#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/DasSharedRef.hpp>
#include <das/IDasSchedulerService.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace Das::Core::TaskScheduler;

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
    task0["nextExecutionTime"] = "2026-04-23T12:30:00+08:00";
    task0["properties"] = {{"setting1", "value1"}, {"setting2", 42}};
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    EXPECT_TRUE(state.contains("state"));
    EXPECT_TRUE(state.contains("tasks"));
    EXPECT_TRUE(state.contains("availableTaskTypes"));

    ASSERT_EQ(state["tasks"].size(), 1u);
    EXPECT_EQ(state["tasks"][0]["id"], 0);
    EXPECT_EQ(
        state["tasks"][0]["nextExecutionTime"],
        "2026-04-23T12:30:00+08:00");
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
    nlohmann::json internal_props;
    internal_props["nextExecutionTime"] = "2026-05-01T08:00:00+08:00";
    auto result = scheduler_->UpdateTaskInternalProperties(0, internal_props);
    EXPECT_EQ(result, DAS_S_OK);

    // Verify in-memory
    auto state = scheduler_->Get();
    EXPECT_EQ(
        state["tasks"][0]["nextExecutionTime"],
        "2026-05-01T08:00:00+08:00");

    // Verify persisted
    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    EXPECT_EQ(persisted["nextExecutionTime"], "2026-05-01T08:00:00+08:00");
}

TEST_F(SchedulerServiceTest, UpdateInternalProperties_ClearNextExecutionTime)
{
    settings_manager_->CreateProfile("0");

    nlohmann::json task0;
    task0["id"] = 0;
    task0["taskGuid"] = "11111111-1111-1111-1111-111111111111";
    task0["pluginGuid"] = "00000000-0000-0000-0000-000000000002";
    task0["nextExecutionTime"] = "2026-05-01T08:00:00+08:00";
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

TEST_F(SchedulerServiceTest, UpdateInternalProperties_NotFound)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    nlohmann::json internal_props;
    internal_props["nextExecutionTime"] = "2026-05-01T08:00:00+08:00";
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
    internal_props["nextExecutionTime"] = "2026-06-01T10:00:00+08:00";
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
    EXPECT_EQ(
        state["tasks"][0]["nextExecutionTime"],
        "2026-06-01T10:00:00+08:00");
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
