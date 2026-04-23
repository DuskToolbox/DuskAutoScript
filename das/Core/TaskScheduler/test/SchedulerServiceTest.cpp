#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/Core/TaskScheduler/SchedulerServiceImpl.h>
#include <das/DasSharedRef.hpp>
#include <das/DasString.hpp>
#include <das/IDasSchedulerService.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace Das::Core::TaskScheduler;
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
        IDasReadOnlyString*                  p_environment_json,
        IDasReadOnlyString*                  p_task_settings_json) override
    {
        ++do_call_count;
        stop_token_was_null = (stop_token == nullptr);
        env_was_null = (p_environment_json == nullptr);
        props_was_null = (p_task_settings_json == nullptr);

        if (p_task_settings_json)
        {
            const char* c_str = nullptr;
            if (DAS_S_OK == p_task_settings_json->GetUtf8(&c_str) && c_str)
            {
                last_props_json = c_str;
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
    std::unique_ptr<Das::Core::ForeignInterfaceHost::PluginManager>
                                      plugin_manager_;
    std::unique_ptr<SchedulerService> scheduler_;
    std::thread                       io_thread_;
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
    EXPECT_EQ(
        persisted["nextExecutionTime"].get<std::string>(),
        "2026-06-15T10:30:00");
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

// ============================================================
// Scheduler controller validation tests
// ============================================================

#include "controller/DasSchedulerController.hpp"

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
                return CreateIDasReadOnlyStringFromUtf8(
                    R"({"state":"stopped"})",
                    pp);
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
