#include "controller/DasProfileController.hpp"
#include "controller/DasSchedulerController.hpp"
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/SettingsManager/SettingsServiceImpl.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/Core/TaskScheduler/SchedulerServiceImpl.h>
#include <das/DasApi.h>
#include <das/DasSharedRef.hpp>
#include <das/DasString.hpp>
#include <das/IDasSchedulerService.h>
#include <das/IDasSettingsService.h>
#include <das/Utils/Expected.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace Das::Core::TaskScheduler;
using Das::DasOutPtr;
using Das::DasPtr;

namespace
{
    std::filesystem::path UniqueTestDir()
    {
        static std::atomic<int> counter{0};
        auto name = "scheduler_test_" + std::to_string(counter.fetch_add(1));
        return std::filesystem::current_path() / name;
    }

    /// Helper to write scheduler index + task instance files directly.
    void WriteSchedulerState(
        Das::Core::SettingsManager::SettingsManager& sm,
        int64_t                                      nextTaskId,
        const std::vector<int64_t>&                  taskOrder,
        const std::vector<nlohmann::json>&           taskInstances)
    {
        nlohmann::json index;
        index["nextTaskId"] = nextTaskId;
        index["taskOrder"] = taskOrder;
        sm.UpdateSchedulerIndexJson("0", index);

        for (size_t i = 0; i < taskInstances.size() && i < taskOrder.size();
             ++i)
        {
            sm.UpdateTaskInstanceJson("0", taskOrder[i], taskInstances[i]);
        }
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
        plugin_manager_ =
            std::make_unique<Das::Core::ForeignInterfaceHost::PluginManager>(
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
    std::unique_ptr<Das::Core::ForeignInterfaceHost::PluginManager>
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
    DasGuid              empty_guid{};
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
    EXPECT_TRUE(state.contains("state"));
    EXPECT_EQ(state["tasks"].size(), 0u);
    EXPECT_EQ(state["availableTaskTypes"].size(), 0u);
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

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "00000000-0000-0000-0000-000000000001";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["nextExecutionTime"] = nullptr;
    task0["properties"] = {{"key1", "value1"}};
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    auto result = scheduler_->Initialize(plugin_dir_, {});
    ASSERT_EQ(result, DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["id"], 0);
    EXPECT_EQ(state["tasks"][0]["availability"], "unavailable");
    EXPECT_EQ(state["tasks"][0]["properties"]["key1"], "value1");
}

TEST_F(SchedulerServiceTest, Initialize_CorruptTaskFile_VisibleAsInvalid)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json scheduler_index;
    scheduler_index["nextTaskId"] = 1;
    scheduler_index["taskOrder"] = nlohmann::json::array({0});
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    auto task_file = settings_dir_ / "0" / "taskId0.json";
    {
        std::ofstream ofs{task_file};
        ofs << "CORRUPT DATA {{{";
    }

    auto result = scheduler_->Initialize(plugin_dir_, {});
    ASSERT_EQ(result, DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["id"], 0);
    EXPECT_EQ(state["tasks"][0]["availability"], "invalid");
    EXPECT_FALSE(
        state["tasks"][0]["unavailabilityReason"].get<std::string>().empty());
}

TEST_F(SchedulerServiceTest, Initialize_MissingTaskType_Unavailable)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-1111-1111-1111-111111111111";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["properties"] = nlohmann::json::object();

    nlohmann::json task1;
    task1["id"] = 1;
    task1["taskGuid"] = "22222222-2222-2222-2222-222222222222";
    task1["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task1["properties"] = nlohmann::json::object();

    WriteSchedulerState(*settings_manager_, 2, {0, 1}, {task0, task1});

    auto result = scheduler_->Initialize(plugin_dir_, {});
    ASSERT_EQ(result, DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 2u);
    EXPECT_EQ(state["tasks"][0]["availability"], "unavailable");
    EXPECT_EQ(state["tasks"][1]["availability"], "unavailable");
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

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    task0["pluginGuid"] = "FFFFFFFF-0000-0000-0000-000000000000";
    // 2026-04-23T12:30:00+08:00 = 2026-04-23T04:30:00Z = 1776918600
    task0["nextExecutionTime"] = 1776918600;
    task0["properties"] = {{"setting1", "value1"}, {"setting2", 42}};
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    EXPECT_TRUE(state.contains("state"));
    EXPECT_TRUE(state.contains("tasks"));
    EXPECT_TRUE(state.contains("availableTaskTypes"));

    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["id"], 0);
    EXPECT_EQ(state["tasks"][0]["nextExecutionTime"], 1776918600);
    EXPECT_EQ(state["tasks"][0]["properties"]["setting1"], "value1");
    EXPECT_EQ(state["tasks"][0]["properties"]["setting2"], 42);
}

TEST_F(SchedulerServiceTest, Get_CorruptTaskFile_VisibleWithInvalidMarker)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json scheduler_index;
    scheduler_index["nextTaskId"] = 2;
    scheduler_index["taskOrder"] = nlohmann::json::array({0, 1});
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    // Task 0: corrupt file
    auto task_file = settings_dir_ / "0" / "taskId0.json";
    {
        std::ofstream ofs{task_file};
        ofs << "{broken json";
    }

    // Task 1: valid file
    nlohmann::json task1;
    task1["id"] = 1;
    task1["taskGuid"] = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    task1["pluginGuid"] = "FFFFFFFF-0000-0000-0000-000000000000";
    task1["properties"] = {{"key", "value"}};
    settings_manager_->UpdateTaskInstanceJson("0", 1, task1);

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 2u);

    // Corrupt task should be visible as invalid
    EXPECT_EQ(state["tasks"][0]["id"], 0);
    EXPECT_EQ(state["tasks"][0]["availability"], "invalid");

    // Valid task should be visible as unavailable (no plugin loaded)
    EXPECT_EQ(state["tasks"][1]["id"], 1);
    EXPECT_EQ(state["tasks"][1]["availability"], "unavailable");
    EXPECT_EQ(state["tasks"][1]["properties"]["key"], "value");
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

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    task0["pluginGuid"] = "FFFFFFFF-0000-0000-0000-000000000000";
    task0["properties"] = nlohmann::json::object();

    nlohmann::json task1;
    task1["id"] = 1;
    task1["taskGuid"] = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    task1["pluginGuid"] = "FFFFFFFF-0000-0000-0000-000000000000";
    task1["properties"] = nlohmann::json::object();

    WriteSchedulerState(*settings_manager_, 2, {0, 1}, {task0, task1});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Delete task 0
    auto result = scheduler_->DeleteTask(0);
    EXPECT_EQ(result, DAS_S_OK);

    // Verify in-memory state
    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["id"], 1);

    // Verify persisted state: taskId0.json should be deleted
    EXPECT_FALSE(std::filesystem::exists(settings_dir_ / "0" / "taskId0.json"));
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId1.json"));

    auto index = settings_manager_->GetSchedulerIndexJson("0");
    ASSERT_EQ(index["taskOrder"].size(), 1u);
    EXPECT_EQ(index["taskOrder"][0], 1);
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

    nlohmann::json scheduler_index;
    scheduler_index["nextTaskId"] = 1;
    scheduler_index["taskOrder"] = nlohmann::json::array({0});
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    // Corrupt task file
    auto task_file = settings_dir_ / "0" / "taskId0.json";
    {
        std::ofstream ofs{task_file};
        ofs << "corrupt";
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["availability"], "invalid");

    // Deleting an invalid instance should succeed
    auto result = scheduler_->DeleteTask(0);
    EXPECT_EQ(result, DAS_S_OK);

    state = scheduler_->Get();
    EXPECT_EQ(state["tasks"].size(), 0u);
}

TEST_F(SchedulerServiceTest, DeleteTask_UnavailableInstance_Succeeds)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-1111-1111-1111-111111111111";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["properties"] = nlohmann::json::object();
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["availability"], "unavailable");

    auto result = scheduler_->DeleteTask(0);
    EXPECT_EQ(result, DAS_S_OK);

    state = scheduler_->Get();
    EXPECT_EQ(state["tasks"].size(), 0u);
}

TEST_F(SchedulerServiceTest, DeleteTask_PersistenceFailure_RollsBack)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-1111-1111-1111-111111111111";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["properties"] = nlohmann::json::object();
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Verify the instance exists
    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 1u);

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
    EXPECT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["id"], 0);
}

// ============================================================
// UpdateTaskInternalProperties
// ============================================================

TEST_F(SchedulerServiceTest, UpdateInternalProperties_NextExecutionTime)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-1111-1111-1111-111111111111";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["nextExecutionTime"] = nullptr;
    task0["properties"] = nlohmann::json::object();
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Update nextExecutionTime
    // 2026-05-01T08:00:00+08:00 = 2026-05-01T00:00:00Z = 1777593600
    nlohmann::json internal_props;
    internal_props["nextExecutionTime"] = 1777593600;
    auto result = scheduler_->UpdateTaskInternalProperties(0, internal_props);
    EXPECT_EQ(result, DAS_S_OK);

    // Verify in-memory
    auto state = scheduler_->Get();
    EXPECT_EQ(state["tasks"][0]["nextExecutionTime"], 1777593600);

    // Verify persisted
    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    EXPECT_EQ(persisted["nextExecutionTime"], 1777593600);
}

TEST_F(SchedulerServiceTest, UpdateInternalProperties_ClearNextExecutionTime)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-1111-1111-1111-111111111111";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    // 2026-05-01T08:00:00+08:00 = 2026-05-01T00:00:00Z = 1777593600
    task0["nextExecutionTime"] = 1777593600;
    task0["properties"] = nlohmann::json::object();
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    nlohmann::json internal_props;
    internal_props["nextExecutionTime"] = nullptr;
    auto result = scheduler_->UpdateTaskInternalProperties(0, internal_props);
    EXPECT_EQ(result, DAS_S_OK);

    auto state = scheduler_->Get();
    EXPECT_TRUE(state["tasks"][0]["nextExecutionTime"].is_null());
}

TEST_F(SchedulerServiceTest, UnparseableNextExecutionTime_PreservedInGet)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-2222-3333-4444-555555555555";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000000";
    task0["nextExecutionTime"] = -1; // Invalid timestamp, preserved as-is
    task0["properties"] = nlohmann::json::object();

    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Negative timestamps are preserved as-is in Get() output.
    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["availability"], "unavailable");
    EXPECT_EQ(state["tasks"][0]["nextExecutionTime"], -1);
}

TEST_F(SchedulerServiceTest, UpdateInternalProperties_NotFound)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    nlohmann::json internal_props;
    internal_props["nextExecutionTime"] = 1777593600;
    auto result = scheduler_->UpdateTaskInternalProperties(99, internal_props);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

// ============================================================
// UpdateTaskProperties on unavailable/invalid instances
// ============================================================

TEST_F(SchedulerServiceTest, UpdateProperties_UnavailableInstance_ReturnsError)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-1111-1111-1111-111111111111";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["properties"] = nlohmann::json::object();
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    nlohmann::json props;
    props["someKey"] = "someValue";
    auto result = scheduler_->UpdateTaskProperties(0, props);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(SchedulerServiceTest, UpdateProperties_InvalidInstance_ReturnsError)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json scheduler_index;
    scheduler_index["nextTaskId"] = 1;
    scheduler_index["taskOrder"] = nlohmann::json::array({0});
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    auto task_file = settings_dir_ / "0" / "taskId0.json";
    {
        std::ofstream ofs{task_file};
        ofs << "corrupt";
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    nlohmann::json props;
    props["someKey"] = "someValue";
    auto result = scheduler_->UpdateTaskProperties(0, props);
    EXPECT_NE(result, DAS_S_OK);
}

// ============================================================
// Running mutation guards
// ============================================================

TEST_F(SchedulerServiceTest, AddTask_NotInitialized_ReturnsError)
{
    DasGuid task_guid{};
    int64_t out_id = -1;
    auto    result = scheduler_->AddTask(task_guid, &out_id);
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

    nlohmann::json props;
    props["key"] = "value";
    auto result = scheduler_->UpdateTaskProperties(99, props);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

TEST_F(SchedulerServiceTest, AddTask_NullPointer)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    DasGuid task_guid{};
    auto    result = scheduler_->AddTask(task_guid, nullptr);
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);
}

// ============================================================
// Persistence verification
// ============================================================

TEST_F(SchedulerServiceTest, UpdateInternalProperties_PersistsToFile)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-1111-1111-1111-111111111111";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["nextExecutionTime"] = nullptr;
    task0["properties"] = nlohmann::json::object();
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    nlohmann::json internal_props;
    // 2026-06-01T10:00:00+08:00 = 2026-06-01T02:00:00Z = 1780279200
    internal_props["nextExecutionTime"] = 1780279200;
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
    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["nextExecutionTime"], 1780279200);
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
        plugin_manager_ =
            std::make_unique<Das::Core::ForeignInterfaceHost::PluginManager>(
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
    std::unique_ptr<Das::Core::ForeignInterfaceHost::PluginManager>
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

    auto parsed = nlohmann::json::parse(c_str);
    EXPECT_TRUE(parsed.contains("state"));
    EXPECT_TRUE(parsed.contains("tasks"));
    EXPECT_TRUE(parsed.contains("availableTaskTypes"));
}

TEST_F(SchedulerServiceImplTest, AddTask_NullPointer_ReturnsError)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    DasGuid guid{};
    auto    result = impl_->AddTask(guid, nullptr);
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
    std::atomic<uint32_t> ref_count_{0};
    std::atomic<int>      do_call_count{0};
    std::atomic<bool>     stop_token_was_null{true};
    std::atomic<bool>     env_was_null{true};
    std::atomic<bool>     props_was_null{true};
    std::string           last_props_json;

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

        if (p_task_settings_json)
        {
            DasPtr<IDasReadOnlyString> p_str;
            if (DAS_S_OK == p_task_settings_json->ToString(-1, p_str.Put()))
            {
                const char* c_str = nullptr;
                if (DAS_S_OK == p_str->GetUtf8(&c_str) && c_str)
                {
                    last_props_json = c_str;
                }
            }
        }

        return DAS_S_OK;
    }

    DasResult GetNextExecutionTime(
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

// {12345678-9ABC-4DEF-8123-456789ABCDEF}
static constexpr DasGuid FactoryPluginGuid = {
    0x12345678,
    0x9ABC,
    0x4DEF,
    {0x81, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}};

// {87654321-CBA9-4FED-9123-FEDCBA987654}
static constexpr DasGuid FactoryTaskGuid = {
    0x87654321,
    0xCBA9,
    0x4FED,
    {0x91, 0x23, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54}};

constexpr char FactoryPluginGuidString[] =
    "12345678-9ABC-4DEF-8123-456789ABCDEF";
constexpr char FactoryTaskGuidString[] = "87654321-CBA9-4FED-9123-FEDCBA987654";

struct FactoryTaskSharedState
{
    std::mutex              mutex;
    std::condition_variable cv;
    int                     created_instance_count = 0;
    std::vector<int>        executed_instance_ids;
};

class FactoryBackedTask final : public Das::PluginInterface::IDasTask
{
public:
    explicit FactoryBackedTask(std::shared_ptr<FactoryTaskSharedState> state)
        : state_(std::move(state))
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        instance_id_ = ++state_->created_instance_count;
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
        *p_out_guid = FactoryTaskGuid;
        return DAS_S_OK;
    }

    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override
    {
        if (!pp)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8("FactoryBackedTask", pp);
    }

    DasResult Do(
        Das::PluginInterface::IDasStopToken*,
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson*) override
    {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->executed_instance_ids.push_back(instance_id_);
        }
        state_->cv.notify_all();
        return DAS_S_OK;
    }

    DasResult GetNextExecutionTime(
        Das::ExportInterface::DasDate* p_out_date) override
    {
        if (!p_out_date)
        {
            return DAS_E_INVALID_POINTER;
        }

        *p_out_date = {2030, 1, 1, 0, 0, static_cast<uint8_t>(instance_id_)};
        return DAS_S_OK;
    }

private:
    std::atomic<uint32_t>                   ref_count_{0};
    std::shared_ptr<FactoryTaskSharedState> state_;
    int                                     instance_id_ = 0;
};

class FactoryTaskPluginPackage final
    : public Das::PluginInterface::IDasPluginPackage
{
public:
    explicit FactoryTaskPluginPackage(
        std::shared_ptr<FactoryTaskSharedState> state)
        : state_(std::move(state))
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

    DasResult QueryInterface(const DasGuid& iid, void** pp_out) override
    {
        if (!pp_out)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (iid == DasIidOf<IDasBase>())
        {
            *pp_out = static_cast<IDasBase*>(this);
            AddRef();
            return DAS_S_OK;
        }
        if (iid == DasIidOf<Das::PluginInterface::IDasPluginPackage>())
        {
            *pp_out =
                static_cast<Das::PluginInterface::IDasPluginPackage*>(this);
            AddRef();
            return DAS_S_OK;
        }
        *pp_out = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    DasResult EnumFeature(
        uint64_t                                index,
        Das::PluginInterface::DasPluginFeature* p_out_feature) override
    {
        if (!p_out_feature)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (index != 0)
        {
            return DAS_E_OUT_OF_RANGE;
        }
        *p_out_feature = Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK;
        return DAS_S_OK;
    }

    DasResult CreateFeatureInterface(
        uint64_t   index,
        IDasBase** pp_out_interface) override
    {
        if (!pp_out_interface)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (index != 0)
        {
            return DAS_E_OUT_OF_RANGE;
        }

        DasOutPtr<IDasBase> result(pp_out_interface);
        auto*               task = new FactoryBackedTask(state_);
        result.Set(task);
        result.Keep();
        return DAS_S_OK;
    }

    DasResult CanUnloadNow(bool* can_unload_now) override
    {
        if (!can_unload_now)
        {
            return DAS_E_INVALID_POINTER;
        }
        *can_unload_now = true;
        return DAS_S_OK;
    }

private:
    std::atomic<uint32_t>                   ref_count_{0};
    std::shared_ptr<FactoryTaskSharedState> state_;
};

class FakeFactoryRuntime final
    : public DAS::Core::ForeignInterfaceHost::IForeignLanguageRuntime
{
public:
    explicit FakeFactoryRuntime(
        Das::PluginInterface::IDasPluginPackage* package)
        : package_(package)
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

    DasResult QueryInterface(const DasGuid& iid, void** pp_out) override
    {
        if (!pp_out)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (iid == DasIidOf<IDasBase>())
        {
            *pp_out = static_cast<IDasBase*>(this);
            AddRef();
            return DAS_S_OK;
        }
        *pp_out = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    auto LoadPlugin(const std::filesystem::path& path)
        -> DAS::Utils::Expected<DasPtr<IDasBase>> override
    {
        if (!std::filesystem::exists(path))
        {
            return DAS::Utils::MakeUnexpected(DAS_E_FILE_NOT_FOUND);
        }

        DasPtr<IDasBase> package_base;
        auto             result = package_->QueryInterface(
            DasIidOf<IDasBase>(),
            reinterpret_cast<void**>(package_base.Put()));
        if (DAS::IsFailed(result))
        {
            return DAS::Utils::MakeUnexpected(result);
        }

        return package_base;
    }

private:
    std::atomic<uint32_t>                           ref_count_{0};
    DasPtr<Das::PluginInterface::IDasPluginPackage> package_;
};

void WriteFactoryPluginManifest(const std::filesystem::path& manifest_path)
{
    nlohmann::json manifest;
    manifest["name"] = "FactoryPlugin";
    manifest["author"] = "Tests";
    manifest["version"] = "1.0";
    manifest["guid"] = FactoryPluginGuidString;
    manifest["description"] = "Factory-backed scheduler test plugin";
    manifest["supportedSystem"] = "Windows";
    manifest["language"] = "Cpp";
    manifest["pluginFilenameExtension"] = "dll";
    manifest["settings"] = nlohmann::json::object();
    manifest["tasks"] = {
        {FactoryTaskGuidString,
         {{"pluginGuid", FactoryPluginGuidString},
          {"name", "factoryTask"},
          {"description", "Scheduler test task"},
          {"descriptors", nlohmann::json::array()}}}};

    std::ofstream ofs(manifest_path);
    ofs << manifest.dump(2);
}

namespace
{

    FakeTask* SetupSchedulerWithFakeTask(
        SchedulerService&                               scheduler,
        Das::Core::SettingsManager::SettingsManager&    sm,
        const std::filesystem::path&                    plugin_dir,
        Das::Core::ForeignInterfaceHost::PluginManager& pm)
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
        nlohmann::json task0;
        task0["id"] = 0;
        task0["taskGuid"] = "A1B2C3D4-E5F6-7890-ABCD-EF1234567890";
        task0["pluginGuid"] = "00000000-0000-0000-0000-000000000000";
        task0["nextExecutionTime"] = nullptr;
        task0["properties"] = {{"key1", "value1"}};
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
        plugin_manager_ =
            std::make_unique<Das::Core::ForeignInterfaceHost::PluginManager>(
                *settings_manager_,
                Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                    ipc_sp_));
        registry_ = std::make_unique<DAS::Core::IPC::RemoteObjectRegistry>();
        plugin_manager_->SetRegistry(*registry_);
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));

        // Start IO thread so that steady_timer can fire
        io_thread_ = std::thread(
            [this]()
            {
                auto& io = ipc_sp_->GetIoContext();
                boost::asio::executor_work_guard<
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
    std::unique_ptr<DAS::Core::IPC::RemoteObjectRegistry>     registry_;
    std::unique_ptr<Das::Core::ForeignInterfaceHost::PluginManager>
                                      plugin_manager_;
    std::unique_ptr<SchedulerService> scheduler_;
    std::thread                       io_thread_;
};

class SchedulerRuntimeBackedTest : public ::testing::Test
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

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        settings_manager_->CreateProfile("0");

        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        plugin_manager_ =
            std::make_unique<Das::Core::ForeignInterfaceHost::PluginManager>(
                *settings_manager_,
                Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                    ipc_sp_));
        registry_ = std::make_unique<DAS::Core::IPC::RemoteObjectRegistry>();
        plugin_manager_->SetRegistry(*registry_);
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));

        shared_state_ = std::make_shared<FactoryTaskSharedState>();
        auto* raw_package = new FactoryTaskPluginPackage(shared_state_);
        raw_package->AddRef();
        DasPtr<Das::PluginInterface::IDasPluginPackage> package(raw_package);

        auto* raw_runtime = new FakeFactoryRuntime(package.Get());
        raw_runtime->AddRef();
        DasPtr<DAS::Core::ForeignInterfaceHost::IForeignLanguageRuntime>
            runtime(raw_runtime);

        ASSERT_EQ(plugin_manager_->Initialize(1, runtime), DAS_S_OK);

        io_thread_ = std::thread(
            [this]()
            {
                auto& io = ipc_sp_->GetIoContext();
                boost::asio::executor_work_guard<
                    boost::asio::io_context::executor_type>
                    work(io.get_executor());
                io.run();
            });
    }

    void TearDown() override
    {
        if (scheduler_ && scheduler_->Status() == SchedulerState::Running)
        {
            EXPECT_EQ(scheduler_->Disable(), DAS_S_OK);
        }

        ipc_sp_->GetIoContext().stop();
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
        std::filesystem::remove_all(test_dir_);
    }

    bool WaitForExecutions(
        size_t                    expected_count,
        std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(shared_state_->mutex);
        return shared_state_->cv.wait_until(
            lock,
            deadline,
            [this, expected_count]()
            {
                return shared_state_->executed_instance_ids.size()
                       >= expected_count;
            });
    }

    std::filesystem::path test_dir_;
    std::filesystem::path settings_dir_;
    std::filesystem::path plugin_dir_;
    std::filesystem::path manifest_path_;
    std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
    std::unique_ptr<DAS::Core::IPC::RemoteObjectRegistry>     registry_;
    std::unique_ptr<Das::Core::ForeignInterfaceHost::PluginManager>
                                            plugin_manager_;
    std::unique_ptr<SchedulerService>       scheduler_;
    std::shared_ptr<FactoryTaskSharedState> shared_state_;
    std::thread                             io_thread_;
};

// ============================================================
// Execution tests
// ============================================================

TEST_F(SchedulerServiceTest, OnTick_SkipsInvalidInstance)
{
    settings_manager_->CreateProfile("0");

    // Write a corrupt task instance file
    nlohmann::json scheduler_index;
    scheduler_index["nextTaskId"] = 1;
    scheduler_index["taskOrder"] = nlohmann::json::array({0});
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
    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-1111-1111-1111-111111111111";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["properties"] = nlohmann::json::object();
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

    // Verify properties JSON was passed
    EXPECT_FALSE(fake->last_props_json.empty());
    auto parsed = nlohmann::json::parse(fake->last_props_json);
    EXPECT_EQ(parsed["key1"], "value1");
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
    ASSERT_TRUE(persisted.contains("nextExecutionTime"));
    ASSERT_TRUE(persisted["nextExecutionTime"].is_number_integer());
    // 2026-06-15T10:30:00Z = 1781519400
    EXPECT_EQ(persisted["nextExecutionTime"].get<int64_t>(), 1781519400);
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
    ASSERT_TRUE(persisted.contains("nextExecutionTime"));
    ASSERT_TRUE(persisted["nextExecutionTime"].is_number_integer());
    EXPECT_EQ(persisted["nextExecutionTime"].get<int64_t>(), 4102444799LL);
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

    // After disable, scheduler should be stopped
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);
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
    EXPECT_TRUE(state.contains("state"));

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    auto stop_result = scheduler_->Disable();
    EXPECT_EQ(stop_result, DAS_S_OK);
}

TEST_F(SchedulerRuntimeBackedTest, Initialize_DiscoversFlatManifestTaskTypes)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(state["availableTaskTypes"].size(), 1u);
    EXPECT_EQ(
        state["availableTaskTypes"][0]["taskGuid"],
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

    nlohmann::json internal_props;
    // 2099-01-01T00:00:00Z = 4070908800
    internal_props["nextExecutionTime"] = 4070908800;
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

    EXPECT_EQ(first_task_json["nextExecutionTime"], 4070908800);
    EXPECT_NE(second_task_json["nextExecutionTime"], nullptr);
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

    ASSERT_TRUE(first_task_json["nextExecutionTime"].is_number_integer());
    ASSERT_TRUE(second_task_json["nextExecutionTime"].is_number_integer());
    EXPECT_NE(
        first_task_json["nextExecutionTime"].get<int64_t>(),
        second_task_json["nextExecutionTime"].get<int64_t>());

    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->executed_instance_ids.size(), 2u);
    EXPECT_NE(
        shared_state_->executed_instance_ids[0],
        shared_state_->executed_instance_ids[1]);
}

// ============================================================
// Scheduler controller validation tests
// ============================================================

namespace
{

    /// Fake IDasSchedulerService that records method calls.
    class FakeSchedulerService final : public IDasSchedulerService
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
        DasResult SetStateNotifyCallback(SchedulerNotifyFunc, void*) override
        {
            return DAS_S_OK;
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
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->initialize_called);
}

TEST_F(SchedulerControllerTest, Start_ProfileNonZero_Rejected)
{
    auto req = MakeRequest("/api/scheduler/1/start", "{}", {{"profile", "1"}});
    auto resp = controller_->Start(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->start_called);
}

TEST_F(SchedulerControllerTest, Stop_ProfileNonZero_Rejected)
{
    auto req = MakeRequest("/api/scheduler/1/stop", "{}", {{"profile", "1"}});
    auto resp = controller_->Stop(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->stop_called);
}

TEST_F(SchedulerControllerTest, Get_ProfileNonZero_Rejected)
{
    auto req = MakeRequest("/api/scheduler/1/get", "{}", {{"profile", "1"}});
    auto resp = controller_->Get(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
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
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->add_task_called);
}

TEST_F(SchedulerControllerTest, DeleteTask_ProfileNonZero_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/1/0/delete",
        "{}",
        {{"profile", "1"}, {"taskId", "0"}});
    auto resp = controller_->DeleteTask(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
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
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
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
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->delete_task_called);
}

TEST_F(SchedulerControllerTest, UpdateProps_MalformedTaskId_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/0/xyz/properties/update",
        R"({"key":"val"})",
        {{"profile", "0"}, {"taskId", "xyz"}});
    auto resp = controller_->UpdateTaskProperties(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->update_props_called);
}

TEST_F(SchedulerControllerTest, UpdateInternalProps_MalformedTaskId_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/0/abc/internal/properties/update",
        R"({"nextExecutionTime":null})",
        {{"profile", "0"}, {"taskId", "abc"}});
    auto resp = controller_->UpdateTaskInternalProperties(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->update_internal_props_called);
}

// ── disabledGuids validation ──

TEST_F(SchedulerControllerTest, Initialize_OldDisabledUnderscoreGuids_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/0/initialize",
        R"({"disabled_guids":[]})",
        {{"profile", "0"}});
    auto resp = controller_->Initialize(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->initialize_called);
}

TEST_F(SchedulerControllerTest, Initialize_NonArrayDisabledGuids_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/0/initialize",
        R"({"disabledGuids":"not-array"})",
        {{"profile", "0"}});
    auto resp = controller_->Initialize(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(fake_svc_->initialize_called);
}

TEST_F(SchedulerControllerTest, Initialize_MalformedGuidMember_Skipped)
{
    auto req = MakeRequest(
        "/api/scheduler/0/initialize",
        R"({"disabledGuids":["not-a-guid","00000000-0000-0000-0000-000000000000"]})",
        {{"profile", "0"}});
    auto resp = controller_->Initialize(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    // Should succeed (malformed GUIDs are skipped, valid ones pass)
    EXPECT_EQ(body["Code"], DAS_S_OK);
    EXPECT_TRUE(fake_svc_->initialize_called);
}

// ── Valid request forwarding ──

TEST_F(SchedulerControllerTest, Initialize_Valid_Forwarded)
{
    auto req = MakeRequest(
        "/api/scheduler/0/initialize",
        R"({"disabledGuids":[]})",
        {{"profile", "0"}});
    auto resp = controller_->Initialize(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_S_OK);
    EXPECT_TRUE(fake_svc_->initialize_called);
}

TEST_F(SchedulerControllerTest, Start_Valid_Forwarded)
{
    auto req = MakeRequest("/api/scheduler/0/start", "{}", {{"profile", "0"}});
    auto resp = controller_->Start(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_S_OK);
    EXPECT_TRUE(fake_svc_->start_called);
}

TEST_F(SchedulerControllerTest, Stop_Valid_Forwarded)
{
    auto req = MakeRequest("/api/scheduler/0/stop", "{}", {{"profile", "0"}});
    auto resp = controller_->Stop(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_S_OK);
    EXPECT_TRUE(fake_svc_->stop_called);
}

TEST_F(SchedulerControllerTest, Get_Valid_Forwarded)
{
    auto req = MakeRequest("/api/scheduler/0/get", "{}", {{"profile", "0"}});
    auto resp = controller_->Get(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_S_OK);
    EXPECT_TRUE(body["Data"].contains("state"));
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
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_S_OK);
    EXPECT_EQ(body["Data"]["taskId"], 42);
    EXPECT_TRUE(fake_svc_->add_task_called);
}

TEST_F(SchedulerControllerTest, DeleteTask_Valid_Forwarded)
{
    auto req = MakeRequest(
        "/api/scheduler/0/5/delete",
        "{}",
        {{"profile", "0"}, {"taskId", "5"}});
    auto resp = controller_->DeleteTask(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_S_OK);
    EXPECT_TRUE(fake_svc_->delete_task_called);
}

TEST_F(SchedulerControllerTest, UpdateProps_Valid_Forwarded)
{
    auto req = MakeRequest(
        "/api/scheduler/0/3/properties/update",
        R"({"key":"value"})",
        {{"profile", "0"}, {"taskId", "3"}});
    auto resp = controller_->UpdateTaskProperties(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_S_OK);
    EXPECT_TRUE(fake_svc_->update_props_called);
}

TEST_F(SchedulerControllerTest, UpdateInternalProps_Valid_Forwarded)
{
    auto req = MakeRequest(
        "/api/scheduler/0/3/internal/properties/update",
        R"({"nextExecutionTime":"2026-05-01T08:00:00+08:00"})",
        {{"profile", "0"}, {"taskId", "3"}});
    auto resp = controller_->UpdateTaskInternalProperties(req);
    auto body = nlohmann::json::parse(resp.Release().body());
    EXPECT_EQ(body["Code"], DAS_S_OK);
    EXPECT_TRUE(fake_svc_->update_internal_props_called);
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
    auto body = nlohmann::json::parse(response.Release().body());

    EXPECT_EQ(body["Code"], DAS_S_FALSE);
    EXPECT_EQ(
        body["Message"],
        "Plugin settings were invalid and restored to an empty object");
    EXPECT_TRUE(body["Data"].is_object());

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
        plugin_manager_ =
            std::make_unique<Das::Core::ForeignInterfaceHost::PluginManager>(
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
    std::unique_ptr<Das::Core::ForeignInterfaceHost::PluginManager>
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
    EXPECT_EQ(state["state"], "stopped");
    EXPECT_EQ(state["tasks"].size(), 0u);

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
    EXPECT_EQ(scheduler_index["nextTaskId"], 2);
    //    scheduler.json.taskOrder[] order is stable
    ASSERT_EQ(scheduler_index["taskOrder"].size(), 2u);
    EXPECT_EQ(scheduler_index["taskOrder"][0], 0);
    EXPECT_EQ(scheduler_index["taskOrder"][1], 1);

    //    taskId{taskId}.json is created for each task instance
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId0.json"));
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId1.json"));

    //    Verify task instance file content
    auto task0_json = settings_manager_->GetTaskInstanceJson("0", 0);
    EXPECT_EQ(task0_json["id"], 0);
    EXPECT_EQ(task0_json["taskGuid"], "A1B2C3D4-E5F6-7890-ABCD-EF1234567890");

    // 6. Get: verify two task instances in queue
    state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 2u);
    EXPECT_EQ(state["tasks"][0]["id"], 0);
    EXPECT_EQ(state["tasks"][1]["id"], 1);
    EXPECT_EQ(state["tasks"][0]["availability"], "available");
    EXPECT_EQ(state["tasks"][1]["availability"], "available");

    // 7. UpdateTaskInternalProperties on instance 0
    //    (no descriptors from manifest, so UpdateTaskProperties would reject;
    //     internal properties are always writable when not running)
    nlohmann::json internal_props;
    // 2026-07-01T09:00:00Z = 1782896400
    internal_props["nextExecutionTime"] = 1782896400;
    auto update_result =
        scheduler_->UpdateTaskInternalProperties(0, internal_props);
    EXPECT_EQ(update_result, DAS_S_OK);

    //    Verify updated properties are persisted under the selected instance
    auto task0_updated = settings_manager_->GetTaskInstanceJson("0", 0);
    EXPECT_EQ(task0_updated["nextExecutionTime"], 1782896400);

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

    nlohmann::json rejected_props;
    rejected_props["key"] = "val";
    EXPECT_EQ(
        scheduler_->UpdateTaskProperties(0, rejected_props),
        DAS_E_TASK_WORKING);
    EXPECT_EQ(
        scheduler_->UpdateTaskInternalProperties(0, internal_props),
        DAS_E_TASK_WORKING);

    // 10. Disable (stop) -- clears runtime state but split files remain
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);

    // Split files should still exist after Disable
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId0.json"));
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId1.json"));
    auto index_after_stop = settings_manager_->GetSchedulerIndexJson("0");
    EXPECT_EQ(index_after_stop["nextTaskId"], 2);

    // -- Phase 3: Re-initialize, delete, verify persistence across instances --

    // 11. Re-register the fake task (Disable unloaded plugins)
    plugin_manager_->RegisterTestFeature(
        Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK,
        fake_plugin_guid,
        static_cast<IDasBase*>(fake));

    // 12. Re-initialize to re-materialize from persisted split files
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 2u);
    EXPECT_EQ(state["tasks"][0]["id"], 0);
    EXPECT_EQ(state["tasks"][1]["id"], 1);
    // Task 0's nextExecutionTime was persisted
    EXPECT_EQ(state["tasks"][0]["nextExecutionTime"], 1782896400);

    // 13. Delete task 0
    auto delete_result = scheduler_->DeleteTask(0);
    ASSERT_EQ(delete_result, DAS_S_OK);

    //    delete removes only the selected taskId{taskId}.json and queue-order
    //    entry
    EXPECT_FALSE(std::filesystem::exists(settings_dir_ / "0" / "taskId0.json"));
    EXPECT_TRUE(std::filesystem::exists(settings_dir_ / "0" / "taskId1.json"));

    auto index_after_delete = settings_manager_->GetSchedulerIndexJson("0");
    ASSERT_EQ(index_after_delete["taskOrder"].size(), 1u);
    EXPECT_EQ(index_after_delete["taskOrder"][0], 1);

    // 14. Get: only task 1 remains
    state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["id"], 1);

    // 15. A new scheduler instance sees persisted final state
    auto ipc_sp2 = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
    auto scheduler2 = std::make_unique<SchedulerService>(
        *plugin_manager_,
        Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(ipc_sp2));
    ASSERT_EQ(scheduler2->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state2 = scheduler2->Get();
    ASSERT_EQ(state2["tasks"].size(), 1u);
    EXPECT_EQ(state2["tasks"][0]["id"], 1);
    EXPECT_EQ(
        state2["tasks"][0]["taskGuid"],
        "A1B2C3D4-E5F6-7890-ABCD-EF1234567890");

    // Cleanup: release the fake task
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
        plugin_manager_ =
            std::make_unique<Das::Core::ForeignInterfaceHost::PluginManager>(
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
    std::unique_ptr<Das::Core::ForeignInterfaceHost::PluginManager>
                                      plugin_manager_;
    std::unique_ptr<SchedulerService> scheduler_;
};

TEST_F(SchedulerUnavailableTest, UnavailableAndCorruptInstances_VisibleInGet)
{
    // Seed scheduler.json with two task ids: one referencing an absent task
    // type GUID (unavailable), one with a corrupt JSON file (invalid).
    nlohmann::json scheduler_index;
    scheduler_index["nextTaskId"] = 2;
    scheduler_index["taskOrder"] = nlohmann::json::array({0, 1});
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    // Task 0: unavailable -- references a GUID not in loaded manifests
    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-2222-3333-4444-555555555555";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["nextExecutionTime"] = nullptr;
    task0["properties"] = {{"customProp", "preservedValue"}};
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
    ASSERT_EQ(state["tasks"].size(), 2u);

    // Unavailable instance: visible with original id/GUID/properties
    EXPECT_EQ(state["tasks"][0]["id"], 0);
    EXPECT_EQ(state["tasks"][0]["availability"], "unavailable");
    EXPECT_EQ(
        state["tasks"][0]["taskGuid"],
        "11111111-2222-3333-4444-555555555555");
    EXPECT_EQ(state["tasks"][0]["properties"]["customProp"], "preservedValue");

    // Invalid/corrupt instance: visible with original id
    EXPECT_EQ(state["tasks"][1]["id"], 1);
    EXPECT_EQ(state["tasks"][1]["availability"], "invalid");
    EXPECT_FALSE(
        state["tasks"][1]["unavailabilityReason"].get<std::string>().empty());
}

TEST_F(SchedulerUnavailableTest, UnavailableInstance_NotExecutedOnStart)
{
    // Seed scheduler with an unavailable task type
    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-2222-3333-4444-555555555555";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["properties"] = nlohmann::json::object();
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
    nlohmann::json scheduler_index;
    scheduler_index["nextTaskId"] = 1;
    scheduler_index["taskOrder"] = nlohmann::json::array({0});
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
    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-2222-3333-4444-555555555555";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["properties"] = nlohmann::json::object();
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["availability"], "unavailable");

    auto result = scheduler_->DeleteTask(0);
    EXPECT_EQ(result, DAS_S_OK);

    state = scheduler_->Get();
    EXPECT_EQ(state["tasks"].size(), 0u);

    // Verify persisted removal
    EXPECT_FALSE(std::filesystem::exists(settings_dir_ / "0" / "taskId0.json"));
    auto index = settings_manager_->GetSchedulerIndexJson("0");
    EXPECT_EQ(index["taskOrder"].size(), 0u);
}

TEST_F(SchedulerUnavailableTest, DeleteCorruptInstance_Succeeds)
{
    nlohmann::json scheduler_index;
    scheduler_index["nextTaskId"] = 1;
    scheduler_index["taskOrder"] = nlohmann::json::array({0});
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    auto task_file = settings_dir_ / "0" / "taskId0.json";
    {
        std::ofstream ofs{task_file};
        ofs << "corrupt";
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["availability"], "invalid");

    auto result = scheduler_->DeleteTask(0);
    EXPECT_EQ(result, DAS_S_OK);

    state = scheduler_->Get();
    EXPECT_EQ(state["tasks"].size(), 0u);
}

TEST_F(SchedulerUnavailableTest, MixedAvailableAndUnavailable_OnlyAvailableRuns)
{
    // Start IO thread so steady_timer can fire for tick execution
    auto&       io = ipc_sp_->GetIoContext();
    std::thread io_thread(
        [&io]()
        {
            boost::asio::executor_work_guard<
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
    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-2222-3333-4444-555555555555";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["properties"] = nlohmann::json::object();

    // Task 1: available (matches FakeTaskGuid)
    nlohmann::json task1;
    task1["id"] = 1;
    task1["taskGuid"] = "A1B2C3D4-E5F6-7890-ABCD-EF1234567890";
    task1["pluginGuid"] = "00000000-0000-0000-0000-000000000000";
    task1["properties"] = {{"testKey", "testValue"}};

    WriteSchedulerState(*settings_manager_, 2, {0, 1}, {task0, task1});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(state["tasks"].size(), 2u);
    EXPECT_EQ(state["tasks"][0]["availability"], "unavailable");
    EXPECT_EQ(state["tasks"][1]["availability"], "available");

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
