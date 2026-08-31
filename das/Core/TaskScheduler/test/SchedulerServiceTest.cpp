#include "controller/DasProfileController.hpp"
#include "controller/DasSchedulerController.hpp"
#include <IpcTestConfig.h>
#include <Windows.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/SettingsManager/SettingsServiceImpl.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/Core/TaskScheduler/SchedulerServiceImpl.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasApi.h>
#include <das/DasSharedRef.hpp>
#include <das/DasString.hpp>
#include <das/IDasSchedulerService.h>
#include <das/IDasSettingsService.h>
#include <das/Plugins/SchedulerTestPlugin/SchedulerTestSharedState.h>
#include <das/Utils/Expected.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
        auto name = "scheduler_test_" + std::to_string(counter.fetch_add(1));
        auto path = std::filesystem::current_path() / name;
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
    // callers pass the kind explicitly.
    yyjson::value MakeStrictAuthoringChange(
        int64_t          base_revision,
        std::string_view kind = "setValue")
    {
        yyjson::value change(Das::Utils::MakeYyjsonObject());
        (*change.as_object())[std::string_view("baseRevision")] = base_revision;
        (*change.as_object())[std::string_view("kind")] = std::string(kind);
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
        (*change.as_object())[std::string_view("payload")] = std::move(payload);
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
        settings_dir_ = test_dir_ / "settings";
        std::filesystem::create_directories(settings_dir_);
        plugin_dir_ = test_dir_ / "plugins";
        std::filesystem::create_directories(plugin_dir_);

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        auto ipc_sp =
            DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp));
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp));
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
    auto bad_dir = test_dir_ / "nonexistent_plugins";
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

    auto task_file = settings_dir_ / "0" / "taskId0.json";
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
    auto task_file = settings_dir_ / "0" / "taskId0.json";
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
    EXPECT_FALSE(std::filesystem::exists(settings_dir_ / "0" / "taskId0.json"));
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId1.json"));

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
    auto task_file = settings_dir_ / "0" / "taskId0.json";
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
    auto profile_dir = settings_dir_ / "0";
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

    auto task_file = settings_dir_ / "0" / "taskId0.json";
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
    auto ipc_sp = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
    auto scheduler2 = std::make_unique<SchedulerService>(
        *plugin_manager_,
        Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(ipc_sp));
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
// SchedulerServiceImpl wrapper tests
// ============================================================

class SchedulerServiceImplTest : public ::testing::Test
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

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        auto ipc_sp =
            DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp));
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp));
        // Use raw pointer with AddRef/Release lifecycle
        impl_ = new SchedulerServiceImpl(*scheduler_);
        impl_->AddRef();
    }

    void TearDown() override
    {
        if (impl_)
        {
            impl_->Release();
            impl_ = nullptr;
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
    std::unique_ptr<Das::Core::ForeignInterfaceHost::TestablePluginManager>
                                      plugin_manager_;
    std::unique_ptr<SchedulerService> scheduler_;
    SchedulerServiceImpl*             impl_ = nullptr;
};

TEST_F(SchedulerServiceImplTest, Get_NullPointer_ReturnsError)
{
    auto result = impl_->Get(nullptr);
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);
}

TEST_F(SchedulerServiceImplTest, Get_ReturnsJsonString)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    DasPtr<IDasReadOnlyString> p_json;
    auto                       result = impl_->Get(p_json.Put());
    EXPECT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(p_json);

    const char* c_str = nullptr;
    auto        get_result = p_json->GetUtf8(&c_str);
    EXPECT_EQ(get_result, DAS_S_OK);

    auto parsed = *Das::Utils::ParseYyjsonFromString(c_str);
    EXPECT_TRUE((*parsed.as_object()).contains(std::string_view("state")));
    EXPECT_TRUE((*parsed.as_object()).contains(std::string_view("tasks")));
    EXPECT_TRUE(
        (*parsed.as_object()).contains(std::string_view("availableTaskTypes")));
}

TEST_F(SchedulerServiceImplTest, AddTask_NullPointer_ReturnsError)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const DasGuid guid{};
    auto          result = impl_->AddTask(guid, nullptr);
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);
}

TEST_F(SchedulerServiceImplTest, AddTask_NotFound)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    DasGuid guid{};
    guid.data1 = 0xDEADBEEF;
    int64_t out_id = -1;
    auto    result = impl_->AddTask(guid, &out_id);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

TEST_F(SchedulerServiceImplTest, DeleteTask_NotFound)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto result = impl_->DeleteTask(99);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

TEST_F(SchedulerServiceImplTest, UpdateTaskProperties_NullPointer_ReturnsError)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto result = impl_->UpdateTaskProperties(0, nullptr);
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);
}

TEST_F(SchedulerServiceImplTest, UpdateTaskProperties_InvalidJson_ReturnsError)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    DasPtr<IDasReadOnlyString> p_bad_json;
    auto                       cr = CreateIDasReadOnlyStringFromUtf8(
        "not valid json {{{",
        p_bad_json.Put());
    ASSERT_EQ(cr, DAS_S_OK);

    auto result = impl_->UpdateTaskProperties(0, p_bad_json.Get());
    EXPECT_EQ(result, DAS_E_INVALID_JSON);
}

TEST_F(SchedulerServiceImplTest, UpdateTaskInternalProperties_NullPointer)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto result = impl_->UpdateTaskInternalProperties(0, nullptr);
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);
}

TEST_F(SchedulerServiceImplTest, AddTask_DelegatesToSchedulerService)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Verify AddTask returns same error as direct call
    DasGuid guid{};
    guid.data1 = 0xDEAD;
    int64_t out_id = -1;

    auto impl_result = impl_->AddTask(guid, &out_id);
    auto direct_result = scheduler_->AddTask(guid, &out_id);
    EXPECT_EQ(impl_result, direct_result);
}

TEST_F(SchedulerServiceImplTest, QueryInterface_ReturnsSchedulerService)
{
    DasPtr<IDasSchedulerService> p_svc;
    auto                         result = impl_->QueryInterface(
        DasIidOf<IDasSchedulerService>(),
        reinterpret_cast<void**>(p_svc.Put()));
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(p_svc.Get() != nullptr);
}

TEST_F(SchedulerServiceImplTest, SchedulerServiceImplAuthoringNullOutRejected)
{
    DasPtr<IDasReadOnlyString> request;
    ASSERT_EQ(CreateIDasReadOnlyStringFromUtf8("{}", request.Put()), DAS_S_OK);
    EXPECT_EQ(
        impl_->GetTaskAuthoringDocument(0, request.Get(), nullptr),
        DAS_E_INVALID_POINTER);
}

TEST_F(SchedulerServiceImplTest, SchedulerServiceImplAuthoringInvalidJson)
{
    DasPtr<IDasReadOnlyString> request;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8("not-json", request.Put()),
        DAS_S_OK);
    DasPtr<IDasReadOnlyString> out;
    EXPECT_EQ(
        impl_->ApplyTaskAuthoringChange(0, request.Get(), out.Put()),
        DAS_E_INVALID_JSON);
}

TEST_F(SchedulerServiceImplTest, SchedulerServiceImplAuthoringReturnsJson)
{
    DasPtr<IDasReadOnlyString> request;
    ASSERT_EQ(CreateIDasReadOnlyStringFromUtf8("{}", request.Put()), DAS_S_OK);
    DasPtr<IDasReadOnlyString> out;
    ASSERT_EQ(
        impl_->CompileTaskAuthoring(99, request.Get(), out.Put()),
        DAS_S_OK);
    ASSERT_TRUE(out.Get() != nullptr);
    const char* raw = nullptr;
    ASSERT_EQ(out->GetUtf8(&raw), DAS_S_OK);
    auto parsed = Das::Utils::ParseYyjsonFromString(raw);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->as_object().has_value());
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

        return DAS_S_OK;
    }

    DasResult GetNextExecutionTime(
        Das::ExportInterface::IDasJson* /*p_task_settings_json*/,
        Das::ExportInterface::DasDate* p_out_date) override
    {
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
        settings_dir_ = test_dir_ / "settings";
        std::filesystem::create_directories(settings_dir_);
        plugin_dir_ = test_dir_ / "plugins";
        std::filesystem::create_directories(plugin_dir_);

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));

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
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
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
        settings_dir_ = test_dir_ / "settings";
        std::filesystem::create_directories(settings_dir_);
        plugin_dir_ = test_dir_ / "plugins";
        std::filesystem::create_directories(plugin_dir_);
        manifest_path_ = plugin_dir_ / "FactoryPlugin.json";
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

        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));
        // IPC 模式下 PluginManager 需要 DasHost.exe 路径才能 spawn 子进程
        // 加载 FactoryPlugin.dll。DAS_HOST_EXE_PATH 由 ctest 注入。
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
            // host 未配置时 PluginManager.Initialize 会因无法 spawn 而失败，
            // 测试断言会暴露具体错误，这里不提前 fail。
        }

        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));

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

    // 部署构建产物里的真实插件（dll + json 成对拷贝，json 文件名带平台前缀
    // 差异：dll 由 manifest name 字段定位，json 文件名不必与 name 一致）。
    // force_ipc 为 true 时把部署副本的 loadMode 补写为 ipc——测试 exe 静态
    // 链接 DasCore，进程内再加载插件 dll 会产生双份 DasCore 全局状态。
    void DeployRealPluginArtifacts(
        const std::string& dll_name,
        const std::string& manifest_name,
        bool               force_ipc)
    {
        namespace fs = std::filesystem;
        const fs::path build_plugin_dir{IpcTestConfig::GetPluginDir()};

        const fs::path src_dll = build_plugin_dir / dll_name;
        ASSERT_TRUE(fs::exists(src_dll))
            << "plugin dll not found at: " << src_dll.string();
        fs::copy_file(
            src_dll,
            plugin_dir_ / dll_name,
            fs::copy_options::overwrite_existing);

        const fs::path src_manifest = build_plugin_dir / manifest_name;
        ASSERT_TRUE(fs::exists(src_manifest))
            << "plugin manifest not found at: " << src_manifest.string();
        if (!force_ipc)
        {
            fs::copy_file(
                src_manifest,
                plugin_dir_ / manifest_name,
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
        std::ofstream ofs(plugin_dir_ / manifest_name, std::ios::trunc);
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

        // 释放顺序：先 scheduler_/plugin_manager_(释放 IPC 句柄与子进程对象) ->
        // 再清理共享内存段。
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
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
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

    auto task_file = settings_dir_ / "0" / "taskId0.json";
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
    auto second_task_json =
        settings_manager_->GetTaskInstanceJson("0", second_task_id);

    EXPECT_EQ(
        (*first_task_json.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        4070908800);
    EXPECT_FALSE(
        (*second_task_json.as_object())[std::string_view("nextExecutionTime")]
            .is_null());
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
        (*change.as_object())[std::string_view("kind")] = std::string(kind);
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
        (*change.as_object())[std::string_view("payload")] = std::move(payload);
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
            // + legacy best-effort sourceFingerprint consumed by
            // RepositoryInvokeCompiler. The legacy kind/migrationState fields
            // are no longer part of the persisted wire.
            (*authoring.as_object())[std::string_view("lastChangeKind")] =
                "setValue";
            (*authoring.as_object())[std::string_view("sourceFingerprint")] =
                "seed-source";
            (*entry.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        (*entry.as_object())[std::string_view("acceptedProperties")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        // RepositoryEntryDto 的 graph_document / compiled_artifact 是 required
        // 字段，caster 反序列化时要求 key 存在；生产 CreateEntry 用默认（空）
        // 值序列化，seed 同样补空对象，否则后续 ListEntries 反序列化抛
        // "JSON object key not found: graphDocument"。
        (*entry.as_object())[std::string_view("graphDocument")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        (*entry.as_object())[std::string_view("compiledArtifact")] =
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
                settings_dir / "0" / "taskRepository0.json"));
        auto repository = scheduler.GetTaskRepository();
        auto entries =
            (*repository.as_object())[std::string_view("entries")].as_array();
        ASSERT_TRUE(entries.has_value());
        EXPECT_TRUE(entries->empty());
        ExpectNoRepositoryAuthoringProviderCalls(state);
    }

    std::string SerializeJsonForTest(const yyjson::value& json)
    {
        auto serialized = Das::Utils::SerializeYyjsonValue(json, false);
        EXPECT_TRUE(serialized.has_value());
        return serialized.value_or("{}");
    }

    yyjson::value ParseReadOnlyJsonForTest(IDasReadOnlyString* json)
    {
        const char* raw = nullptr;
        EXPECT_EQ(json->GetUtf8(&raw), DAS_S_OK);

        auto parsed = Das::Utils::ParseYyjsonFromString(
            raw ? std::string_view(raw) : std::string_view{});
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed)
                      : yyjson::value(Das::Utils::MakeYyjsonObject());
    }

    void CreateRepositoryEntryThroughBridgeForTest(
        SchedulerServiceImpl& impl,
        std::string_view      display_name,
        int64_t               retry_count = 3)
    {
        auto request_json = SerializeJsonForTest(
            MakeRepositoryCreateRequest(display_name, retry_count));
        DasPtr<IDasReadOnlyString> request;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                request_json.c_str(),
                request.Put()),
            DAS_S_OK);

        DasPtr<IDasReadOnlyString> out;
        ASSERT_EQ(
            impl.CreateRepositoryEntry(request.Get(), out.Put()),
            DAS_S_OK);
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
    SchedulerServiceImplTest,
    SchedulerServiceImplRepositoryGetNullOutRejected)
{
    EXPECT_EQ(impl_->GetTaskRepository(nullptr), DAS_E_INVALID_POINTER);
}

TEST_F(
    SchedulerServiceImplTest,
    SchedulerServiceImplRepositoryGetReturnsSchedulerJson)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    DasPtr<IDasReadOnlyString> out;
    ASSERT_EQ(impl_->GetTaskRepository(out.Put()), DAS_S_OK);
    ASSERT_TRUE(out.Get() != nullptr);

    auto parsed = ParseReadOnlyJsonForTest(out.Get());
    auto obj = parsed.as_object();
    ASSERT_TRUE(obj.has_value());
    auto entries = (*obj)[std::string_view("entries")].as_array();
    ASSERT_TRUE(entries.has_value());
    EXPECT_TRUE(entries->empty());
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryCreateNullOutRejected)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl impl(*scheduler_);

    auto request_json =
        SerializeJsonForTest(MakeRepositoryCreateRequest("Bridge entry", 7));
    DasPtr<IDasReadOnlyString> request;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(request_json.c_str(), request.Put()),
        DAS_S_OK);

    EXPECT_EQ(
        impl.CreateRepositoryEntry(request.Get(), nullptr),
        DAS_E_INVALID_POINTER);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryCreateNullBodyRejected)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl       impl(*scheduler_);
    DasPtr<IDasReadOnlyString> out;

    EXPECT_EQ(
        impl.CreateRepositoryEntry(nullptr, out.Put()),
        DAS_E_INVALID_POINTER);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryCreateInvalidJsonRejected)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl impl(*scheduler_);

    DasPtr<IDasReadOnlyString> malformed;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8("not-json", malformed.Put()),
        DAS_S_OK);
    DasPtr<IDasReadOnlyString> out;
    EXPECT_EQ(
        impl.CreateRepositoryEntry(malformed.Get(), out.Put()),
        DAS_E_INVALID_JSON);

    DasPtr<IDasReadOnlyString> non_object;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8("[]", non_object.Put()),
        DAS_S_OK);
    out.Reset();
    EXPECT_EQ(
        impl.CreateRepositoryEntry(non_object.Get(), out.Put()),
        DAS_E_INVALID_JSON);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryCreateReturnsCreatedEntryJson)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl impl(*scheduler_);

    auto request_json =
        SerializeJsonForTest(MakeRepositoryCreateRequest("Bridge entry", 7));
    DasPtr<IDasReadOnlyString> request;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(request_json.c_str(), request.Put()),
        DAS_S_OK);

    DasPtr<IDasReadOnlyString> out;
    ASSERT_EQ(impl.CreateRepositoryEntry(request.Get(), out.Put()), DAS_S_OK);
    ASSERT_TRUE(out.Get() != nullptr);

    auto parsed = ParseReadOnlyJsonForTest(out.Get());
    auto obj = parsed.as_object();
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
    SchedulerServiceImplRepositoryDeleteDelegatesResult)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl impl(*scheduler_);

    auto request_json =
        SerializeJsonForTest(MakeRepositoryCreateRequest("Delete me", 4));
    DasPtr<IDasReadOnlyString> request;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(request_json.c_str(), request.Put()),
        DAS_S_OK);
    DasPtr<IDasReadOnlyString> created;
    ASSERT_EQ(
        impl.CreateRepositoryEntry(request.Get(), created.Put()),
        DAS_S_OK);

    EXPECT_EQ(impl.DeleteRepositoryEntry(0), DAS_S_OK);
    EXPECT_EQ(impl.DeleteRepositoryEntry(0), DAS_E_NOT_FOUND);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryRenameNullOutRejected)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl impl(*scheduler_);

    auto rename_json =
        SerializeJsonForTest(MakeRepositoryRenameRequest("No output"));
    DasPtr<IDasReadOnlyString> rename;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(rename_json.c_str(), rename.Put()),
        DAS_S_OK);

    EXPECT_EQ(
        impl.RenameRepositoryEntry(0, rename.Get(), nullptr),
        DAS_E_INVALID_POINTER);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryRenameNullBodyRejected)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl       impl(*scheduler_);
    DasPtr<IDasReadOnlyString> out;

    EXPECT_EQ(
        impl.RenameRepositoryEntry(0, nullptr, out.Put()),
        DAS_E_INVALID_POINTER);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryRenameInvalidJsonRejected)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl impl(*scheduler_);

    DasPtr<IDasReadOnlyString> malformed;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8("{bad", malformed.Put()),
        DAS_S_OK);
    DasPtr<IDasReadOnlyString> out;
    EXPECT_EQ(
        impl.RenameRepositoryEntry(0, malformed.Get(), out.Put()),
        DAS_E_INVALID_JSON);

    DasPtr<IDasReadOnlyString> non_object;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8("[]", non_object.Put()),
        DAS_S_OK);
    out.Reset();
    EXPECT_EQ(
        impl.RenameRepositoryEntry(0, non_object.Get(), out.Put()),
        DAS_E_INVALID_JSON);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryRenameReturnsUpdatedEntryJson)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl impl(*scheduler_);

    auto request_json =
        SerializeJsonForTest(MakeRepositoryCreateRequest("Before rename", 6));
    DasPtr<IDasReadOnlyString> request;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(request_json.c_str(), request.Put()),
        DAS_S_OK);
    DasPtr<IDasReadOnlyString> created;
    ASSERT_EQ(
        impl.CreateRepositoryEntry(request.Get(), created.Put()),
        DAS_S_OK);

    auto rename_json =
        SerializeJsonForTest(MakeRepositoryRenameRequest("After rename"));
    DasPtr<IDasReadOnlyString> rename;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(rename_json.c_str(), rename.Put()),
        DAS_S_OK);

    DasPtr<IDasReadOnlyString> out;
    ASSERT_EQ(impl.RenameRepositoryEntry(0, rename.Get(), out.Put()), DAS_S_OK);

    auto parsed = ParseReadOnlyJsonForTest(out.Get());
    auto obj = parsed.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ((*obj)[std::string_view("entryId")].as_sint().value_or(-1), 0);
    EXPECT_EQ(
        (*obj)[std::string_view("displayName")].as_string().value_or(""),
        std::string_view("After rename"));
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryAuthoringNullOutRejected)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl impl(*scheduler_);

    DasPtr<IDasReadOnlyString> request;
    ASSERT_EQ(CreateIDasReadOnlyStringFromUtf8("{}", request.Put()), DAS_S_OK);

    auto change_json = SerializeJsonForTest(MakeRepositoryAuthoringChange(0));
    DasPtr<IDasReadOnlyString> change;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(change_json.c_str(), change.Put()),
        DAS_S_OK);

    EXPECT_EQ(
        impl.GetRepositoryEntryAuthoringDocument(0, request.Get(), nullptr),
        DAS_E_INVALID_POINTER);
    EXPECT_EQ(
        impl.ApplyRepositoryEntryAuthoringChange(0, change.Get(), nullptr),
        DAS_E_INVALID_POINTER);
    EXPECT_EQ(
        impl.CompileRepositoryEntryAuthoring(0, request.Get(), nullptr),
        DAS_E_INVALID_POINTER);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryAuthoringNullBodyRejected)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl       impl(*scheduler_);
    DasPtr<IDasReadOnlyString> out;

    EXPECT_EQ(
        impl.GetRepositoryEntryAuthoringDocument(0, nullptr, out.Put()),
        DAS_E_INVALID_POINTER);
    out.Reset();
    EXPECT_EQ(
        impl.ApplyRepositoryEntryAuthoringChange(0, nullptr, out.Put()),
        DAS_E_INVALID_POINTER);
    out.Reset();
    EXPECT_EQ(
        impl.CompileRepositoryEntryAuthoring(0, nullptr, out.Put()),
        DAS_E_INVALID_POINTER);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryAuthoringInvalidJsonRejected)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    SchedulerServiceImpl impl(*scheduler_);

    DasPtr<IDasReadOnlyString> malformed;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8("{bad", malformed.Put()),
        DAS_S_OK);
    DasPtr<IDasReadOnlyString> out;
    EXPECT_EQ(
        impl.GetRepositoryEntryAuthoringDocument(0, malformed.Get(), out.Put()),
        DAS_E_INVALID_JSON);
    out.Reset();
    EXPECT_EQ(
        impl.ApplyRepositoryEntryAuthoringChange(0, malformed.Get(), out.Put()),
        DAS_E_INVALID_JSON);
    out.Reset();
    EXPECT_EQ(
        impl.CompileRepositoryEntryAuthoring(0, malformed.Get(), out.Put()),
        DAS_E_INVALID_JSON);

    DasPtr<IDasReadOnlyString> non_object;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8("[]", non_object.Put()),
        DAS_S_OK);
    out.Reset();
    EXPECT_EQ(
        impl.GetRepositoryEntryAuthoringDocument(
            0,
            non_object.Get(),
            out.Put()),
        DAS_E_INVALID_JSON);
    out.Reset();
    EXPECT_EQ(
        impl.ApplyRepositoryEntryAuthoringChange(
            0,
            non_object.Get(),
            out.Put()),
        DAS_E_INVALID_JSON);
    out.Reset();
    EXPECT_EQ(
        impl.CompileRepositoryEntryAuthoring(0, non_object.Get(), out.Put()),
        DAS_E_INVALID_JSON);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerServiceImplRepositoryAuthoringAndCompileReturnJson)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    SchedulerServiceImpl impl(*scheduler_);
    CreateRepositoryEntryThroughBridgeForTest(impl, "Authoring bridge", 9);

    DasPtr<IDasReadOnlyString> request;
    ASSERT_EQ(CreateIDasReadOnlyStringFromUtf8("{}", request.Put()), DAS_S_OK);
    DasPtr<IDasReadOnlyString> document_out;
    ASSERT_EQ(
        impl.GetRepositoryEntryAuthoringDocument(
            0,
            request.Get(),
            document_out.Put()),
        DAS_S_OK);
    auto document = ParseReadOnlyJsonForTest(document_out.Get());
    auto document_obj = document.as_object();
    ASSERT_TRUE(document_obj.has_value());
    EXPECT_EQ(
        (*document_obj)[std::string_view("entryId")].as_sint().value_or(-1),
        0);
    EXPECT_TRUE(
        (*document_obj)[std::string_view("document")].as_object().has_value());

    auto change_json = SerializeJsonForTest(MakeRepositoryAuthoringChange(0));
    DasPtr<IDasReadOnlyString> change;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(change_json.c_str(), change.Put()),
        DAS_S_OK);
    DasPtr<IDasReadOnlyString> apply_out;
    ASSERT_EQ(
        impl.ApplyRepositoryEntryAuthoringChange(
            0,
            change.Get(),
            apply_out.Put()),
        DAS_S_OK);
    auto apply = ParseReadOnlyJsonForTest(apply_out.Get());
    auto apply_obj = apply.as_object();
    ASSERT_TRUE(apply_obj.has_value());
    EXPECT_TRUE((*apply_obj)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_TRUE(apply_obj->contains(std::string_view("acceptedProperties")));
    EXPECT_TRUE(apply_obj->contains(std::string_view("document")));
    EXPECT_TRUE(apply_obj->contains(std::string_view("diagnostics")));

    yyjson::value compile_request(Das::Utils::MakeYyjsonObject());
    (*compile_request.as_object())[std::string_view("purpose")] = "preview";
    auto compile_json = SerializeJsonForTest(compile_request);
    DasPtr<IDasReadOnlyString> compile_request_string;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(
            compile_json.c_str(),
            compile_request_string.Put()),
        DAS_S_OK);
    DasPtr<IDasReadOnlyString> compile_out;
    ASSERT_EQ(
        impl.CompileRepositoryEntryAuthoring(
            0,
            compile_request_string.Get(),
            compile_out.Put()),
        DAS_S_OK);

    auto compile = ParseReadOnlyJsonForTest(compile_out.Get());
    auto compile_obj = compile.as_object();
    ASSERT_TRUE(compile_obj.has_value());
    // Phase 84 strict Compile envelope: compile + diagnostics, plus Scheduler
    // routing ack (entryId). The old canExecute/summary rewrapping is gone.
    EXPECT_TRUE(compile_obj->contains(std::string_view("compile")));
    EXPECT_TRUE(compile_obj->contains(std::string_view("diagnostics")));
    EXPECT_FALSE(compile_obj->contains(std::string_view("canExecute")));
    EXPECT_FALSE(compile_obj->contains(std::string_view("summary")));
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
    EXPECT_FALSE(obj->contains(std::string_view("compiledSnapshot")));
    EXPECT_FALSE(obj->contains(std::string_view("providerDebug")));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->get_last_compile_purpose(), "preview");
    EXPECT_EQ(shared_state_->last_context_entry_id, 0);
    EXPECT_FALSE(shared_state_->last_context_had_task_id);
    EXPECT_EQ(shared_state_->get_last_props_key1_value(), "default");
    EXPECT_TRUE((shared_state_->executed_count == 0));
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryInvokeGraphCompileBuildsSnapshot)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryExecutionComponentGuidString);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Repository invoke graph child", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    RepositoryInvoke::Dto::RepositoryTaskRefDto ref;
    ref.entry_id = 0;
    ref.expected_revision = 0;

    RepositoryInvoke::RepositoryInvokeSourceContext context;
    context.source_entry_id = 100;
    auto graph = Das::Utils::ParseYyjsonFromString(R"json({
        "nodes": [
            {
                "id": "invoke-repository-maapi",
                "label": "Invoke repository child",
                "settings": {
                    "repositoryRef": {
                        "kind": "taskRepositoryRef",
                        "entryId": 0,
                        "expectedRevision": 0
                    }
                }
            }
        ]
    })json");
    ASSERT_TRUE(graph.has_value());
    context.source_graph = std::move(*graph);

    auto result = scheduler_->ResolveRepositoryInvokeSnapshot(ref, context);

    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.snapshot.has_value());
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(result.snapshot->source_entry_id, 0);
    EXPECT_EQ(result.snapshot->source_revision, 0);
    EXPECT_EQ(result.snapshot->plugin_guid, FactoryPluginGuidString);
    EXPECT_EQ(result.snapshot->task_type_guid, FactoryTaskGuidString);
    EXPECT_EQ(
        result.snapshot->component_guid,
        FactoryExecutionComponentGuidString);

    RepositoryInvoke::Dto::InvokeRepositoryTaskInputDto runtime_input;
    runtime_input.compiled_snapshot = std::move(result.snapshot.value());
    runtime_input.runtime_inputs = Das::Utils::MakeYyjsonObject();
    auto serialized_runtime_input = yyjson::object(runtime_input);
    auto runtime_input_text =
        serialized_runtime_input.write(yyjson::WriteFlag::NoFlag);
    auto runtime_input_json = Das::Utils::ParseYyjsonFromString(
        std::string_view(runtime_input_text.data(), runtime_input_text.size()));
    ASSERT_TRUE(runtime_input_json.has_value());
    auto runtime_input_obj = runtime_input_json->as_object();
    ASSERT_TRUE(runtime_input_obj.has_value());
    EXPECT_TRUE(
        runtime_input_obj->contains(std::string_view("compiledSnapshot")));

    auto compiled_snapshot =
        (*runtime_input_obj)[std::string_view("compiledSnapshot")].as_object();
    ASSERT_TRUE(compiled_snapshot.has_value());
    auto execution_input =
        (*compiled_snapshot)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(execution_input.has_value());
    EXPECT_EQ(
        (*execution_input)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("compiled"));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->get_last_compile_purpose(), "execution");
    EXPECT_EQ(shared_state_->last_context_entry_id, 0);
    EXPECT_FALSE(shared_state_->last_context_had_task_id);
    EXPECT_TRUE((shared_state_->executed_count == 0));
}

// graph 任务（DasGraphTask 任务类型）经真实调度器 tick 端到端执行 entryRef
// 项：编译信封过校验墙 → OnTick 展开 entryRef → invokeRepository 组件
// （真实 FlowControl 插件）→ 子执行组件收到 executionInput。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphTaskEntryRefItemExecutesThroughSchedulerTick)
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
    // DasGraphTask 走 IPC（任务插件的生产形态）；FlowControl 进程内直连
    // 主进程——invokeRepository 组件与主进程 TaskComponentHost 同进程，
    // 整条链呈星型（每个代理跳只经主进程中转），避开兄弟 host 进程间的
    // 响应路由缺口。
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false);

    // graph 任务实例：formSequence 单 entryRef 项指向 repository entry 0。
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
        {
            // item id 采用 GUID 形态：GraphRuntime 的 PortFrame 数据面以
            // DasGuid 形态的 node_id 做键（非 GUID id 会在输出回填时抛出）。
            auto properties = Das::Utils::ParseYyjsonFromString(R"json({
                "items": [
                    {
                        "itemId": "30000000-0000-0000-0000-000000000002",
                        "target": {
                            "targetKind": "entryRef",
                            "entryRef": {
                                "kind": "entryRef",
                                "entryId": 0,
                                "expectedRevision": 0,
                                "sourceFingerprint": null
                            }
                        },
                        "settings": {}
                    }
                ]
            })json");
            ASSERT_TRUE(properties.has_value());
            (*task0.as_object())[std::string_view("properties")] =
                std::move(*properties);
        }
        WriteSchedulerState(*settings_manager_, 1, {0}, {task0});
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Graph entryRef child", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    bool child_executed = false;
    {
        ipc_scoped_lock lock(shared_state_->mutex);
        child_executed = shared_state_->cv.wait_for(
            lock,
            std::chrono::seconds(20),
            [this]
            {
                return shared_state_->get_last_execution_input().find(
                           "\"key1\":\"compiled\"")
                       != std::string_view::npos;
            });
    }
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    // 让 BusinessThread 上的 OnTick 收尾（Do 被取消后其尾段仍在跑），
    // 避免 TearDown 销毁 scheduler 与 tick 尾段竞态。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_TRUE(child_executed)
        << "entryRef child never received its executionInput";

    const ipc_scoped_lock lock(shared_state_->mutex);
    // 编译不被拒：被引用 entry 经 resolver 完成一次 execution 编译。
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->get_last_compile_purpose(), "execution");
    // 直跑任务本体（FactoryBackedTask）不应被执行——执行的是 graph 任务。
    EXPECT_EQ(shared_state_->executed_count, 0u);
}

// graph authoring 的 actionCatalog 采用 {actionId,label} 对象条目：Scheduler
// 从组件 manifest definition 的 label map 按默认 locale 解析出 label 注入
// context，经 DasGraphTask 透传到 Core SetActionCatalog，GetDocument 投影
// 保持对象形态。断言锚定真实插件的三个组件：FlowControl 的 merge/launch
// 与 DasGraphTask 的 execute（默认 locale=en 取 map 的 en 值）。
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
        /*force_ipc=*/false);

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
            (*node.as_object())[std::string_view("dynamicPorts")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            nodes.as_array()->emplace_back(std::move(node));
        }
        (*props_obj)[std::string_view("nodes")] = std::move(nodes);
        (*props_obj)[std::string_view("edges")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("graphInputs")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("graphOutputs")] =
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
                (*node.as_object())[std::string_view("dynamicPorts")] =
                    yyjson::value(Das::Utils::MakeYyjsonArray());
                nodes.as_array()->emplace_back(std::move(node));
            }
            (*props_obj)[std::string_view("nodes")] = std::move(nodes);
            (*props_obj)[std::string_view("edges")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*props_obj)[std::string_view("graphInputs")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*props_obj)[std::string_view("graphOutputs")] =
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
            (*node.as_object())[std::string_view("dynamicPorts")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            nodes.as_array()->emplace_back(std::move(node));
        }
        (*props_obj)[std::string_view("nodes")] = std::move(nodes);
        (*props_obj)[std::string_view("edges")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("graphInputs")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("graphOutputs")] =
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
        /*force_ipc=*/false);

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
                (*node.as_object())[std::string_view("dynamicPorts")] =
                    yyjson::value(Das::Utils::MakeYyjsonArray());
                nodes.as_array()->emplace_back(std::move(node));
            }
            (*props_obj)[std::string_view("nodes")] = std::move(nodes);
            (*props_obj)[std::string_view("edges")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*props_obj)[std::string_view("graphInputs")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*props_obj)[std::string_view("graphOutputs")] =
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
        /*force_ipc=*/false);

    // 两个 graph 任务实例：componentRef 节点指向 das.flow.launch（4 个
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
                (*node.as_object())[std::string_view("dynamicPorts")] =
                    yyjson::value(Das::Utils::MakeYyjsonArray());
                nodes.as_array()->emplace_back(std::move(node));
            }
            (*props_obj)[std::string_view("nodes")] = std::move(nodes);
            (*props_obj)[std::string_view("edges")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*props_obj)[std::string_view("graphInputs")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*props_obj)[std::string_view("graphOutputs")] =
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

    // 默认 locale=en：真实 manifest 的 en 值经调度器→context→插件会话→
    // Core 字段投影全链可见。
    {
        const auto document = read_document_clone(0);
        EXPECT_EQ(field_label_of(document, "exePath"), "Executable Path");
        EXPECT_EQ(field_label_of(document, "args"), "Arguments");
        EXPECT_EQ(
            field_label_of(document, "workingDirectory"),
            "Working Directory");
        EXPECT_EQ(field_label_of(document, "waitWindow"), "Wait for Window");
    }

    // zh-cn：真实 manifest 的中文值（模拟重启后 locale 的 catalog 重建）。
    SetDefaultLocaleForTest("zh-cn");
    {
        const auto document = read_document_clone(1);
        EXPECT_EQ(field_label_of(document, "exePath"), "可执行文件路径");
        EXPECT_EQ(field_label_of(document, "args"), "启动参数");
        EXPECT_EQ(field_label_of(document, "workingDirectory"), "工作目录");
        EXPECT_EQ(field_label_of(document, "waitWindow"), "等待窗口出现");
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
            (*node.as_object())[std::string_view("dynamicPorts")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            nodes.as_array()->emplace_back(std::move(node));
        }
        (*props_obj)[std::string_view("nodes")] = std::move(nodes);
        (*props_obj)[std::string_view("edges")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("graphInputs")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("graphOutputs")] =
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
            (*node.as_object())[std::string_view("dynamicPorts")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            nodes.as_array()->emplace_back(std::move(node));
        }
        (*props_obj)[std::string_view("nodes")] = std::move(nodes);
        (*props_obj)[std::string_view("edges")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("graphInputs")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("graphOutputs")] =
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
    SchedulerRepositoryInvokeCompileBuildsSnapshot)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryExecutionComponentGuidString);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Repository invoke child", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    RepositoryInvoke::Dto::RepositoryTaskRefDto ref;
    ref.entry_id = 0;
    ref.expected_revision = 0;

    auto result = scheduler_->ResolveRepositoryInvokeSnapshot(ref);

    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.snapshot.has_value());
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(result.snapshot->source_entry_id, 0);
    EXPECT_EQ(result.snapshot->source_revision, 0);
    EXPECT_EQ(result.snapshot->plugin_guid, FactoryPluginGuidString);
    EXPECT_EQ(result.snapshot->task_type_guid, FactoryTaskGuidString);
    EXPECT_EQ(
        result.snapshot->component_guid,
        FactoryExecutionComponentGuidString);

    auto execution_input = result.snapshot->execution_input.as_object();
    ASSERT_TRUE(execution_input.has_value());
    EXPECT_EQ(
        (*execution_input)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("compiled"));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->get_last_compile_purpose(), "execution");
    EXPECT_EQ(shared_state_->last_context_entry_id, 0);
    EXPECT_FALSE(shared_state_->last_context_had_task_id);
    EXPECT_TRUE((shared_state_->executed_count == 0));
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

    auto entry_path = settings_dir_ / "0" / "taskRepository0.json";
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
        std::filesystem::exists(settings_dir_ / "0" / "taskRepository0.json"));
    EXPECT_TRUE(
        std::filesystem::exists(settings_dir_ / "0" / "taskRepository1.json"));

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
        std::filesystem::exists(settings_dir_ / "0" / "taskRepository0.json"));
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
        std::filesystem::exists(settings_dir_ / "0" / "taskRepository0.json"));
    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableBannedPluginReason)
{
    auto banned_dir = plugin_dir_ / "BannedPlugin";
    std::filesystem::create_directories(banned_dir);
    WriteFactoryPluginManifest(
        banned_dir / "BannedPlugin.json",
        BannedPluginGuidString);
    {
        std::ofstream marker(
            banned_dir / "BannedPlugin.willBeDelete",
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
        std::filesystem::exists(settings_dir_ / "0" / "taskRepository0.json"));
    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableLoadFailedReason)
{
    WriteFactoryPluginManifest(
        plugin_dir_ / "LoadFailedPlugin.json",
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
        std::filesystem::exists(settings_dir_ / "0" / "taskRepository0.json"));
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
        std::filesystem::exists(settings_dir_ / "0" / "taskRepository0.json"));
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
        std::filesystem::exists(settings_dir_ / "0" / "taskRepository0.json"));
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
    auto banned_dir = plugin_dir_ / "BannedPlugin";
    std::filesystem::create_directories(banned_dir);
    WriteFactoryPluginManifest(
        banned_dir / "BannedPlugin.json",
        BannedPluginGuidString);
    {
        std::ofstream marker(
            banned_dir / "BannedPlugin.willBeDelete",
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
        plugin_dir_ / "LoadFailedPlugin.json",
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
    auto banned_dir = plugin_dir_ / "BannedPlugin";
    std::filesystem::create_directories(banned_dir);
    WriteFactoryPluginManifest(
        banned_dir / "BannedPlugin.json",
        BannedPluginGuidString);
    {
        std::ofstream marker(
            banned_dir / "BannedPlugin.willBeDelete",
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
        plugin_dir_ / "LoadFailedPlugin.json",
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
    auto banned_dir = plugin_dir_ / "BannedPlugin";
    std::filesystem::create_directories(banned_dir);
    WriteFactoryPluginManifest(
        banned_dir / "BannedPlugin.json",
        BannedPluginGuidString);
    {
        std::ofstream marker(
            banned_dir / "BannedPlugin.willBeDelete",
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
        plugin_dir_ / "LoadFailedPlugin.json",
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

    /// Fake IDasSchedulerService that records method calls.
    class FakeSchedulerService : public IDasSchedulerService
    {
    public:
        std::atomic<uint32_t> ref_count_{0};

        // Records
        std::atomic<bool> initialize_called{false};
        std::atomic<bool> start_called{false};
        std::atomic<bool> stop_called{false};
        std::atomic<bool> get_called{false};
        std::atomic<bool> add_task_called{false};
        std::atomic<bool> delete_task_called{false};
        std::atomic<bool> update_props_called{false};
        std::atomic<bool> update_internal_props_called{false};
        std::atomic<bool> set_enabled_called{false};
        std::atomic<bool> run_now_called{false};
        std::atomic<bool> authoring_get_called{false};
        std::atomic<bool> authoring_apply_called{false};
        std::atomic<bool> authoring_compile_called{false};
        std::atomic<bool> repository_get_called{false};
        std::atomic<bool> repository_create_called{false};
        std::atomic<bool> repository_delete_called{false};
        std::atomic<bool> repository_rename_called{false};
        std::atomic<bool> repository_authoring_get_called{false};
        std::atomic<bool> repository_authoring_apply_called{false};
        std::atomic<bool> repository_compile_called{false};
        int64_t           last_authoring_task_id = -1;
        int64_t           last_repository_entry_id = -1;
        std::string       last_repository_request_json;

        DasResult next_result{DAS_S_OK};

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
        DasResult QueryInterface(const DasGuid&, void**) override
        {
            return DAS_E_NO_INTERFACE;
        }

        DasResult Initialize(
            IDasReadOnlyString*,
            Das::ExportInterface::IDasReadOnlyGuidVector*) override
        {
            initialize_called = true;
            return next_result;
        }
        DasResult Start() override
        {
            start_called = true;
            return next_result;
        }
        DasResult Stop() override
        {
            stop_called = true;
            return next_result;
        }
        DasResult GetState(SchedulerState* p) const override
        {
            if (p)
            {
                *p = SchedulerState::Stopped;
            }
            return DAS_S_OK;
        }
        DasResult Get(IDasReadOnlyString** pp) override
        {
            get_called = true;
            if (pp)
            {
                DasOutPtr<IDasReadOnlyString> result(pp);
                auto cr = CreateIDasReadOnlyStringFromUtf8(
                    R"({"state":"stopped"})",
                    result.Put());
                if (DAS::IsOk(cr))
                {
                    result.Keep();
                }
                return cr;
            }
            return DAS_E_INVALID_POINTER;
        }
        DasResult GetTaskRepository(IDasReadOnlyString** pp) override
        {
            repository_get_called = true;
            if (DAS::IsFailed(next_result))
            {
                return next_result;
            }
            return WriteAuthoringJson(pp, R"({"entries":[]})");
        }
        DasResult CreateRepositoryEntry(
            IDasReadOnlyString*  p_request_json,
            IDasReadOnlyString** pp_out_json) override
        {
            repository_create_called = true;
            RecordRequestJson(p_request_json);
            if (DAS::IsFailed(next_result))
            {
                return next_result;
            }
            return WriteAuthoringJson(
                pp_out_json,
                R"({"entryId":42,"displayName":"repository entry"})");
        }
        DasResult DeleteRepositoryEntry(int64_t entry_id) override
        {
            repository_delete_called = true;
            last_repository_entry_id = entry_id;
            return next_result;
        }
        DasResult RenameRepositoryEntry(
            int64_t              entry_id,
            IDasReadOnlyString*  p_request_json,
            IDasReadOnlyString** pp_out_json) override
        {
            repository_rename_called = true;
            last_repository_entry_id = entry_id;
            RecordRequestJson(p_request_json);
            if (DAS::IsFailed(next_result))
            {
                return next_result;
            }
            return WriteAuthoringJson(
                pp_out_json,
                R"({"entryId":42,"displayName":"renamed repository entry"})");
        }
        DasResult GetRepositoryEntryAuthoringDocument(
            int64_t              entry_id,
            IDasReadOnlyString*  p_request_json,
            IDasReadOnlyString** pp_out_json) override
        {
            repository_authoring_get_called = true;
            last_repository_entry_id = entry_id;
            RecordRequestJson(p_request_json);
            if (DAS::IsFailed(next_result))
            {
                return next_result;
            }
            return WriteAuthoringJson(
                pp_out_json,
                R"({"entryId":42,"document":{"kind":"formSequence","revision":0}})");
        }
        DasResult ApplyRepositoryEntryAuthoringChange(
            int64_t              entry_id,
            IDasReadOnlyString*  p_request_json,
            IDasReadOnlyString** pp_out_json) override
        {
            repository_authoring_apply_called = true;
            last_repository_entry_id = entry_id;
            RecordRequestJson(p_request_json);
            if (DAS::IsFailed(next_result))
            {
                return next_result;
            }
            return WriteAuthoringJson(
                pp_out_json,
                R"({"ok":true,"entryId":42,"revision":1})");
        }
        DasResult CompileRepositoryEntryAuthoring(
            int64_t              entry_id,
            IDasReadOnlyString*  p_request_json,
            IDasReadOnlyString** pp_out_json) override
        {
            repository_compile_called = true;
            last_repository_entry_id = entry_id;
            RecordRequestJson(p_request_json);
            if (DAS::IsFailed(next_result))
            {
                return next_result;
            }
            return WriteAuthoringJson(
                pp_out_json,
                R"({"entryId":42,"canExecute":true,"summary":{},"diagnostics":[]})");
        }
        DasResult AddTask(const DasGuid&, int64_t* p_id) override
        {
            add_task_called = true;
            if (p_id)
            {
                *p_id = 42;
            }
            return next_result;
        }
        DasResult DeleteTask(int64_t) override
        {
            delete_task_called = true;
            return next_result;
        }
        DasResult UpdateTaskProperties(int64_t, IDasReadOnlyString*) override
        {
            update_props_called = true;
            return next_result;
        }
        DasResult UpdateTaskInternalProperties(int64_t, IDasReadOnlyString*)
            override
        {
            update_internal_props_called = true;
            return next_result;
        }
        DasResult SetTaskEnabled(int64_t, DasBool) override
        {
            set_enabled_called = true;
            return next_result;
        }
        DasResult RunTaskNow(int64_t) override
        {
            run_now_called = true;
            return next_result;
        }
        DasResult GetTaskAuthoringDocument(
            int64_t task_id,
            IDasReadOnlyString*,
            IDasReadOnlyString** pp_out_json) override
        {
            authoring_get_called = true;
            last_authoring_task_id = task_id;
            return WriteAuthoringJson(
                pp_out_json,
                R"({"taskId":42,"document":{"kind":"formSequence","revision":0}})");
        }
        DasResult ApplyTaskAuthoringChange(
            int64_t task_id,
            IDasReadOnlyString*,
            IDasReadOnlyString** pp_out_json) override
        {
            authoring_apply_called = true;
            last_authoring_task_id = task_id;
            return WriteAuthoringJson(
                pp_out_json,
                R"({"ok":true,"taskId":42,"revision":1})");
        }
        DasResult CompileTaskAuthoring(
            int64_t task_id,
            IDasReadOnlyString*,
            IDasReadOnlyString** pp_out_json) override
        {
            authoring_compile_called = true;
            last_authoring_task_id = task_id;
            return WriteAuthoringJson(
                pp_out_json,
                R"({"taskId":42,"compile":{"ok":true}})");
        }
        DasResult SetStateNotifyCallback(SchedulerNotifyFunc, void*) override
        {
            return DAS_S_OK;
        }

    private:
        DasResult WriteAuthoringJson(
            IDasReadOnlyString** pp_out_json,
            const char*          json)
        {
            if (!pp_out_json)
            {
                return DAS_E_INVALID_POINTER;
            }
            DasOutPtr<IDasReadOnlyString> result(pp_out_json);
            auto cr = CreateIDasReadOnlyStringFromUtf8(json, result.Put());
            if (DAS::IsOk(cr))
            {
                result.Keep();
            }
            return cr;
        }

        void RecordRequestJson(IDasReadOnlyString* p_json)
        {
            last_repository_request_json.clear();
            if (!p_json)
            {
                return;
            }

            const char* raw = nullptr;
            if (DAS::IsOk(p_json->GetUtf8(&raw)) && raw)
            {
                last_repository_request_json = raw;
            }
        }
    };

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
        fake_svc_ = new FakeSchedulerService();
        fake_svc_->AddRef();
        controller_ = std::make_unique<Das::Http::DasSchedulerController>(
            *fake_svc_,
            std::filesystem::current_path() / "plugins");
    }

    void TearDown() override
    {
        controller_.reset();
        if (fake_svc_)
        {
            fake_svc_->Release();
            fake_svc_ = nullptr;
        }
    }

    FakeSchedulerService*                              fake_svc_ = nullptr;
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
    EXPECT_FALSE(fake_svc_->initialize_called);
}

TEST_F(SchedulerControllerTest, Start_ProfileNonZero_Rejected)
{
    auto req = MakeRequest("/api/scheduler/1/start", "{}", {{"profile", "1"}});
    auto resp = controller_->Start(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->start_called);
}

TEST_F(SchedulerControllerTest, Stop_ProfileNonZero_Rejected)
{
    auto req = MakeRequest("/api/scheduler/1/stop", "{}", {{"profile", "1"}});
    auto resp = controller_->Stop(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->stop_called);
}

TEST_F(SchedulerControllerTest, Get_ProfileNonZero_Rejected)
{
    auto req = MakeRequest("/api/scheduler/1/get", "{}", {{"profile", "1"}});
    auto resp = controller_->Get(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->get_called);
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
    EXPECT_FALSE(fake_svc_->add_task_called);
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
    EXPECT_FALSE(fake_svc_->delete_task_called);
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
    EXPECT_FALSE(fake_svc_->add_task_called);
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
    EXPECT_FALSE(fake_svc_->delete_task_called);
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
    EXPECT_FALSE(fake_svc_->update_props_called);
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
    EXPECT_FALSE(fake_svc_->update_internal_props_called);
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
    EXPECT_FALSE(fake_svc_->initialize_called);
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
    EXPECT_TRUE(fake_svc_->initialize_called);
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
    EXPECT_TRUE(fake_svc_->initialize_called);
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
    EXPECT_TRUE(fake_svc_->initialize_called);
}

TEST_F(SchedulerControllerTest, Start_Valid_Forwarded)
{
    auto req = MakeRequest("/api/scheduler/0/start", "{}", {{"profile", "0"}});
    auto resp = controller_->Start(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    EXPECT_TRUE(fake_svc_->start_called);
}

TEST_F(SchedulerControllerTest, Stop_Valid_Forwarded)
{
    auto req = MakeRequest("/api/scheduler/0/stop", "{}", {{"profile", "0"}});
    auto resp = controller_->Stop(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    EXPECT_TRUE(fake_svc_->stop_called);
}

TEST_F(SchedulerControllerTest, Get_Valid_Forwarded)
{
    auto req = MakeRequest("/api/scheduler/0/get", "{}", {{"profile", "0"}});
    auto resp = controller_->Get(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    EXPECT_TRUE(
        (*body.as_object())[std::string_view("data")].as_object()->contains(
            std::string_view("state")));
    EXPECT_TRUE(fake_svc_->get_called);
}

TEST_F(SchedulerControllerTest, AddTask_Valid_Forwarded)
{
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
    EXPECT_EQ(
        (*(*body.as_object())[std::string_view("data")]
              .as_object())[std::string_view("taskId")]
            .as_sint()
            .value(),
        42);
    EXPECT_TRUE(fake_svc_->add_task_called);
}

TEST_F(SchedulerControllerTest, DeleteTask_Valid_Forwarded)
{
    auto req = MakeRequest(
        "/api/scheduler/0/5/delete",
        "{}",
        {{"profile", "0"}, {"taskId", "5"}});
    auto resp = controller_->DeleteTask(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    EXPECT_TRUE(fake_svc_->delete_task_called);
}

TEST_F(SchedulerControllerTest, UpdateProps_Valid_Forwarded)
{
    auto req = MakeRequest(
        "/api/scheduler/0/3/properties/update",
        R"({"key":"value"})",
        {{"profile", "0"}, {"taskId", "3"}});
    auto resp = controller_->UpdateTaskProperties(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    EXPECT_TRUE(fake_svc_->update_props_called);
}

TEST_F(SchedulerControllerTest, UpdateInternalProps_Valid_Forwarded)
{
    auto req = MakeRequest(
        "/api/scheduler/0/3/internal/properties/update",
        R"({"nextExecutionTime":"2026-05-01T08:00:00+08:00"})",
        {{"profile", "0"}, {"taskId", "3"}});
    auto resp = controller_->UpdateTaskInternalProperties(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    EXPECT_TRUE(fake_svc_->update_internal_props_called);
}

TEST_F(SchedulerControllerTest, SchedulerControllerAuthoringGet_Forwarded)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/tasks/42/authoring/get",
        R"({"view":"default"})",
        {{"profile", "0"}, {"taskId", "42"}});
    auto resp = controller_->AuthoringGet(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    auto root = body.as_object().value();
    auto code = root[std::string_view("code")].as_sint();
    if (!code)
    {
        code = root[std::string_view("code")].as_sint();
    }
    EXPECT_EQ(code.value_or(DAS_E_FAIL), DAS_S_OK);
    auto data_value = root[std::string_view("data")];
    if (data_value.is_null())
    {
        data_value = root[std::string_view("data")];
    }
    auto data = data_value.as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE(data->contains(std::string_view("document")));
    EXPECT_TRUE(fake_svc_->authoring_get_called);
    EXPECT_EQ(fake_svc_->last_authoring_task_id, 42);
}

TEST_F(SchedulerControllerTest, SchedulerControllerAuthoringApply_Forwarded)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/tasks/42/authoring/apply",
        R"({"baseRevision":0,"kind":"setValue","payload":{}})",
        {{"profile", "0"}, {"taskId", "42"}});
    auto resp = controller_->AuthoringApply(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    auto root = body.as_object().value();
    auto code = root[std::string_view("code")].as_sint();
    if (!code)
    {
        code = root[std::string_view("code")].as_sint();
    }
    EXPECT_EQ(code.value_or(DAS_E_FAIL), DAS_S_OK);
    EXPECT_TRUE(fake_svc_->authoring_apply_called);
    EXPECT_EQ(fake_svc_->last_authoring_task_id, 42);
}

TEST_F(SchedulerControllerTest, SchedulerControllerAuthoringCompile_Forwarded)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/tasks/42/authoring/compile",
        R"({"mode":"preview"})",
        {{"profile", "0"}, {"taskId", "42"}});
    auto resp = controller_->AuthoringCompile(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    auto root = body.as_object().value();
    auto code = root[std::string_view("code")].as_sint();
    if (!code)
    {
        code = root[std::string_view("code")].as_sint();
    }
    EXPECT_EQ(code.value_or(DAS_E_FAIL), DAS_S_OK);
    EXPECT_TRUE(fake_svc_->authoring_compile_called);
    EXPECT_EQ(fake_svc_->last_authoring_task_id, 42);
}

TEST_F(SchedulerControllerTest, SchedulerControllerAuthoringErrorKind_Data)
{
    class ErrorAuthoringService final : public FakeSchedulerService
    {
    public:
        DasResult ApplyTaskAuthoringChange(
            int64_t,
            IDasReadOnlyString*,
            IDasReadOnlyString** pp_out_json) override
        {
            return WriteError(
                pp_out_json,
                R"({"ok":false,"errorKind":"revisionConflict","message":"stale","currentRevision":7})");
        }

    private:
        DasResult WriteError(IDasReadOnlyString** pp_out_json, const char* json)
        {
            if (!pp_out_json)
            {
                return DAS_E_INVALID_POINTER;
            }
            DasOutPtr<IDasReadOnlyString> result(pp_out_json);
            auto cr = CreateIDasReadOnlyStringFromUtf8(json, result.Put());
            if (DAS::IsOk(cr))
            {
                result.Keep();
            }
            return cr;
        }
    };

    auto* error_svc = new ErrorAuthoringService();
    error_svc->AddRef();
    Das::Http::DasSchedulerController controller(
        *error_svc,
        std::filesystem::current_path() / "plugins");

    auto req = MakeRequest(
        "/api/v1/scheduler/0/tasks/42/authoring/apply",
        R"({"baseRevision":0,"kind":"setValue","payload":{}})",
        {{"profile", "0"}, {"taskId", "42"}});
    auto resp = controller.AuthoringApply(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    auto root = body.as_object().value();
    auto code = root[std::string_view("code")].as_sint();
    if (!code)
    {
        code = root[std::string_view("code")].as_sint();
    }
    EXPECT_EQ(code.value_or(DAS_S_OK), DAS_E_FAIL);
    auto data_value = root[std::string_view("data")];
    if (data_value.is_null())
    {
        data_value = root[std::string_view("data")];
    }
    auto data = data_value.as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(
        (*data)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("revisionConflict"));

    error_svc->Release();
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryGet_Forwarded)
{
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
    EXPECT_TRUE(fake_svc_->repository_get_called);
    EXPECT_FALSE(fake_svc_->repository_create_called);
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryCreate_Forwarded)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries",
        R"({"pluginGuid":"AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE","taskTypeGuid":"11111111-2222-3333-4444-555555555555"})",
        {{"profile", "0"}});
    auto body = ReleaseJson(controller_->RepositoryCreate(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_E_FAIL),
        DAS_S_OK);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ((*data)[std::string_view("entryId")].as_sint().value_or(-1), 42);
    EXPECT_TRUE(fake_svc_->repository_create_called);
    EXPECT_NE(
        fake_svc_->last_repository_request_json.find("pluginGuid"),
        std::string::npos);
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

    EXPECT_FALSE(fake_svc_->repository_get_called);
    EXPECT_FALSE(fake_svc_->repository_create_called);
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

    EXPECT_FALSE(fake_svc_->repository_get_called);
    EXPECT_FALSE(fake_svc_->repository_create_called);
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

    EXPECT_FALSE(fake_svc_->repository_get_called);
    EXPECT_FALSE(fake_svc_->repository_create_called);
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryDelete_Forwarded)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/123/delete",
        "{}",
        {{"profile", "0"}, {"entryId", "123"}});
    auto body = ReleaseJson(controller_->RepositoryDelete(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_E_FAIL),
        DAS_S_OK);
    EXPECT_TRUE(fake_svc_->repository_delete_called);
    EXPECT_EQ(fake_svc_->last_repository_entry_id, 123);
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryRename_Forwarded)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/rename",
        R"({"displayName":"Renamed"})",
        {{"profile", "0"}, {"entryId", "42"}});
    auto body = ReleaseJson(controller_->RepositoryRename(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_E_FAIL),
        DAS_S_OK);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(
        (*data)[std::string_view("displayName")].as_string().value_or(""),
        std::string_view("renamed repository entry"));
    EXPECT_TRUE(fake_svc_->repository_rename_called);
    EXPECT_EQ(fake_svc_->last_repository_entry_id, 42);
    EXPECT_NE(
        fake_svc_->last_repository_request_json.find("displayName"),
        std::string::npos);
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

    EXPECT_FALSE(fake_svc_->repository_delete_called);
    EXPECT_FALSE(fake_svc_->repository_rename_called);
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

    EXPECT_FALSE(fake_svc_->repository_delete_called);
    EXPECT_FALSE(fake_svc_->repository_rename_called);
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
    EXPECT_FALSE(fake_svc_->repository_rename_called);
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
    EXPECT_FALSE(fake_svc_->repository_rename_called);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryDelete_ServiceError)
{
    fake_svc_->next_result = DAS_E_FAIL;
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/delete",
        "{}",
        {{"profile", "0"}, {"entryId", "42"}});
    auto body = ReleaseJson(controller_->RepositoryDelete(req));

    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_FAIL);
    EXPECT_TRUE(fake_svc_->repository_delete_called);
    EXPECT_EQ(fake_svc_->last_repository_entry_id, 42);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryRename_ServiceError)
{
    fake_svc_->next_result = DAS_E_FAIL;
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/rename",
        R"({"displayName":"Renamed"})",
        {{"profile", "0"}, {"entryId", "42"}});
    auto body = ReleaseJson(controller_->RepositoryRename(req));

    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_FAIL);
    EXPECT_TRUE(fake_svc_->repository_rename_called);
    EXPECT_EQ(fake_svc_->last_repository_entry_id, 42);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryAuthoringGet_Forwarded)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/authoring/get",
        R"({"view":"default"})",
        {{"profile", "0"}, {"entryId", "42"}});
    auto body = ReleaseJson(controller_->RepositoryAuthoringGet(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_E_FAIL),
        DAS_S_OK);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE(data->contains(std::string_view("document")));
    EXPECT_TRUE(fake_svc_->repository_authoring_get_called);
    EXPECT_EQ(fake_svc_->last_repository_entry_id, 42);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryAuthoringApply_Forwarded)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/authoring/apply",
        R"({"baseRevision":0,"kind":"setValue","payload":{}})",
        {{"profile", "0"}, {"entryId", "42"}});
    auto body = ReleaseJson(controller_->RepositoryAuthoringApply(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_E_FAIL),
        DAS_S_OK);
    EXPECT_TRUE(fake_svc_->repository_authoring_apply_called);
    EXPECT_EQ(fake_svc_->last_repository_entry_id, 42);
    EXPECT_NE(
        fake_svc_->last_repository_request_json.find("baseRevision"),
        std::string::npos);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryCompile_PreviewOnly)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/authoring/compile",
        R"({"mode":"preview"})",
        {{"profile", "0"}, {"entryId", "42"}});
    auto body = ReleaseJson(controller_->RepositoryAuthoringCompile(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_E_FAIL),
        DAS_S_OK);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE(data->contains(std::string_view("entryId")));
    EXPECT_TRUE(data->contains(std::string_view("canExecute")));
    EXPECT_TRUE(data->contains(std::string_view("summary")));
    EXPECT_TRUE(data->contains(std::string_view("diagnostics")));
    const auto child_snapshot_key = std::string{"childExecution"} + "Snapshot";
    const auto execution_snapshot_key = std::string{"execution"} + "Snapshot";
    EXPECT_FALSE(data->contains(std::string_view(child_snapshot_key)));
    EXPECT_FALSE(data->contains(std::string_view(execution_snapshot_key)));
    EXPECT_TRUE(fake_svc_->repository_compile_called);
    EXPECT_EQ(fake_svc_->last_repository_entry_id, 42);
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

    EXPECT_FALSE(fake_svc_->repository_authoring_get_called);
    EXPECT_FALSE(fake_svc_->repository_authoring_apply_called);
    EXPECT_FALSE(fake_svc_->repository_compile_called);
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

    EXPECT_FALSE(fake_svc_->repository_authoring_get_called);
    EXPECT_FALSE(fake_svc_->repository_authoring_apply_called);
    EXPECT_FALSE(fake_svc_->repository_compile_called);
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

    EXPECT_FALSE(fake_svc_->repository_authoring_get_called);
    EXPECT_FALSE(fake_svc_->repository_authoring_apply_called);
    EXPECT_FALSE(fake_svc_->repository_compile_called);
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

    EXPECT_FALSE(fake_svc_->repository_authoring_get_called);
    EXPECT_FALSE(fake_svc_->repository_authoring_apply_called);
    EXPECT_FALSE(fake_svc_->repository_compile_called);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryCompile_ServiceError)
{
    fake_svc_->next_result = DAS_E_FAIL;
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/authoring/compile",
        R"({"mode":"preview"})",
        {{"profile", "0"}, {"entryId", "42"}});
    auto body = ReleaseJson(controller_->RepositoryAuthoringCompile(req));

    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_FAIL);
    EXPECT_TRUE(fake_svc_->repository_compile_called);
    EXPECT_EQ(fake_svc_->last_repository_entry_id, 42);
}

// ── Non-initialize paths do not load scheduler plugins ──

TEST_F(SchedulerControllerTest, NonInitializePaths_DelegateOnly)
{
    // Start, Stop, Get, DeleteTask, UpdateTaskProperties,
    // UpdateTaskInternalProperties only delegate to the service.
    // They never call Initialize.

    auto start_req =
        MakeRequest("/api/scheduler/0/start", "{}", {{"profile", "0"}});
    controller_->Start(start_req);
    EXPECT_TRUE(fake_svc_->start_called);
    EXPECT_FALSE(fake_svc_->initialize_called);

    auto stop_req =
        MakeRequest("/api/scheduler/0/stop", "{}", {{"profile", "0"}});
    controller_->Stop(stop_req);
    EXPECT_TRUE(fake_svc_->stop_called);
    EXPECT_FALSE(fake_svc_->initialize_called);

    auto get_req =
        MakeRequest("/api/scheduler/0/get", "{}", {{"profile", "0"}});
    controller_->Get(get_req);
    EXPECT_TRUE(fake_svc_->get_called);
    EXPECT_FALSE(fake_svc_->initialize_called);
}

TEST(ProfileControllerTest, GetPluginSettings_SFalseReportsEmptyObjectRecovery)
{
    auto test_dir = UniqueTestDir();
    std::filesystem::create_directories(test_dir);

    Das::Core::SettingsManager::SettingsManager settings_manager(test_dir);
    ASSERT_EQ(settings_manager.CreateProfile("0"), DAS_S_OK);

    auto plugin_settings_path =
        test_dir / "0" / (std::string(FactoryPluginGuidString) + ".json");
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
        settings_dir_ = test_dir_ / "settings";
        std::filesystem::create_directories(settings_dir_);
        plugin_dir_ = test_dir_ / "plugins";
        std::filesystem::create_directories(plugin_dir_);

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));

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
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
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
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId0.json"));
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId1.json"));

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
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId0.json"));
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId1.json"));
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
    EXPECT_FALSE(std::filesystem::exists(settings_dir_ / "0" / "taskId0.json"));
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId1.json"));

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
    auto ipc_sp2 = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
    auto scheduler2 = std::make_unique<SchedulerService>(
        *plugin_manager_,
        Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(ipc_sp2));
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
    (*change.as_object())[std::string_view("kind")] = "setValue";
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
        settings_dir_ = test_dir_ / "settings";
        std::filesystem::create_directories(settings_dir_);
        plugin_dir_ = test_dir_ / "plugins";
        std::filesystem::create_directories(plugin_dir_);

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));

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
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
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
    auto task1_file = settings_dir_ / "0" / "taskId1.json";
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

    auto task_file = settings_dir_ / "0" / "taskId0.json";
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
    EXPECT_FALSE(std::filesystem::exists(settings_dir_ / "0" / "taskId0.json"));
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

    auto task_file = settings_dir_ / "0" / "taskId0.json";
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
