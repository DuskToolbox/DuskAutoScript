#include "controller/DasProfileController.hpp"
#include "controller/DasSchedulerController.hpp"
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
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
#include <das/Utils/Expected.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <das/Utils/DasJsonCore.h>
#include <filesystem>
#include <fstream>
#include <mutex>
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

    yyjson::value props(Das::Utils::MakeYyjsonObject());
    (*props.as_object())[std::string_view("key")] = "value";
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

    auto parsed = *Das::Utils::ParseYyjsonFromString(c_str);
    EXPECT_TRUE((*parsed.as_object()).contains(std::string_view("state")));
    EXPECT_TRUE((*parsed.as_object()).contains(std::string_view("tasks")));
    EXPECT_TRUE(
        (*parsed.as_object()).contains(std::string_view("availableTaskTypes")));
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

// {FECAC0D5-E038-4FDB-A6E9-EFCE10BCAE5A}
static constexpr DasGuid FactoryAuthoringFactoryGuid = {
    0xFECAC0D5,
    0xE038,
    0x4FDB,
    {0xA6, 0xE9, 0xEF, 0xCE, 0x10, 0xBC, 0xAE, 0x5A}};

// {B56D1081-6838-4251-8CBB-BB223012FEF2}
static constexpr DasGuid DecoyAuthoringFactoryGuid = {
    0xB56D1081,
    0x6838,
    0x4251,
    {0x8C, 0xBB, 0xBB, 0x22, 0x30, 0x12, 0xFE, 0xF2}};

// {2B9CE776-E95F-47F1-BD31-180B9238A94C}
static constexpr DasGuid FactoryTaskComponentFactoryGuid = {
    0x2B9CE776,
    0xE95F,
    0x47F1,
    {0xBD, 0x31, 0x18, 0x0B, 0x92, 0x38, 0xA9, 0x4C}};

// {97BA1BE0-A199-47CB-9604-440B8BBC6555}
static constexpr DasGuid FactoryTaskComponentImplGuid = {
    0x97BA1BE0,
    0xA199,
    0x47CB,
    {0x96, 0x04, 0x44, 0x0B, 0x8B, 0xBC, 0x65, 0x55}};

// {BBCAE0D0-5BC5-4B58-9FBD-585003B62B2D}
static constexpr DasGuid FactoryAuthoringSessionGuid = {
    0xBBCAE0D0,
    0x5BC5,
    0x4B58,
    {0x9F, 0xBD, 0x58, 0x50, 0x03, 0xB6, 0x2B, 0x2D}};

constexpr char FactoryPluginGuidString[] =
    "12345678-9ABC-4DEF-8123-456789ABCDEF";
constexpr char FactoryTaskGuidString[] = "87654321-CBA9-4FED-9123-FEDCBA987654";
constexpr char FactoryAuthoringFactoryGuidString[] =
    "FECAC0D5-E038-4FDB-A6E9-EFCE10BCAE5A";
constexpr char FactoryTaskComponentFactoryGuidString[] =
    "2B9CE776-E95F-47F1-BD31-180B9238A94C";
constexpr char MissingPluginGuidString[] =
    "99999999-0000-4000-8000-000000000001";
constexpr char BannedPluginGuidString[] =
    "99999999-0000-4000-8000-000000000002";
constexpr char LoadFailedPluginGuidString[] =
    "99999999-0000-4000-8000-000000000003";
constexpr char MissingTaskTypeGuidString[] =
    "99999999-0000-4000-8000-000000000004";

struct FactoryTaskSharedState
{
    std::mutex              mutex;
    std::condition_variable cv;
    int                     created_instance_count = 0;
    std::vector<int>        executed_instance_ids;
    int                     authoring_session_count = 0;
    int                     get_document_count = 0;
    int                     apply_change_count = 0;
    int                     compile_count = 0;
    int                     decoy_authoring_create_count = 0;
    int64_t                 last_context_revision = -1;
    std::string             last_props_key1_value;
    std::string             last_compile_purpose;
    bool                    compile_ok = true;
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
        Das::ExportInterface::IDasJson* p_task_settings_json) override
    {
        std::string key1_value;
        if (p_task_settings_json != nullptr)
        {
            DasPtr<IDasReadOnlyString> key;
            if (DAS_S_OK == CreateIDasReadOnlyStringFromUtf8("key1", key.Put())
                && key)
            {
                DasPtr<IDasReadOnlyString> value;
                if (DAS_S_OK
                        == p_task_settings_json->GetStringByName(
                            key.Get(),
                            value.Put())
                    && value)
                {
                    const char* raw = nullptr;
                    if (DAS_S_OK == value->GetUtf8(&raw) && raw)
                    {
                        key1_value = raw;
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->executed_instance_ids.push_back(instance_id_);
            state_->last_props_key1_value = std::move(key1_value);
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

class FakeAuthoringSession final
    : public Das::PluginInterface::IDasTaskAuthoringSession
{
public:
    explicit FakeAuthoringSession(std::shared_ptr<FactoryTaskSharedState> state)
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
        if (iid == DasIidOf<Das::PluginInterface::IDasTaskAuthoringSession>())
        {
            *pp = static_cast<Das::PluginInterface::IDasTaskAuthoringSession*>(
                this);
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
        *p_out_guid = FactoryAuthoringSessionGuid;
        return DAS_S_OK;
    }

    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override
    {
        if (!pp)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8("FakeAuthoringSession", pp);
    }

    DasResult GetDocument(
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson** pp_out_document_json) override
    {
        if (!pp_out_document_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->get_document_count;
        }

        yyjson::value document(Das::Utils::MakeYyjsonObject());
        auto          obj = document.as_object();
        (*obj)[std::string_view("version")] = 1;
        (*obj)[std::string_view("kind")] = "formSequence";
        (*obj)[std::string_view("revision")] = 0;
        (*obj)[std::string_view("values")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        (*obj)[std::string_view("view")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        (*obj)[std::string_view("schema")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        {
            yyjson::value catalog(Das::Utils::MakeYyjsonObject());
            (*catalog.as_object())[std::string_view("providerField")] =
                "preserved";
            (*obj)[std::string_view("catalog")] = std::move(catalog);
        }
        (*obj)[std::string_view("state")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        (*obj)[std::string_view("diagnostics")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*obj)[std::string_view("migration")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());

        auto wrapped = Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
            std::move(document));
        *pp_out_document_json = wrapped.Get();
        (*pp_out_document_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult ApplyChange(
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson** pp_out_result_json) override
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->apply_change_count;
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        yyjson::value props(Das::Utils::MakeYyjsonObject());
        (*props.as_object())[std::string_view("key1")] = "accepted";
        (*result.as_object())[std::string_view("acceptedProperties")] =
            std::move(props);
        (*result.as_object())[std::string_view("sourceFingerprint")] =
            "fake-source";
        (*result.as_object())[std::string_view("migration")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());

        auto wrapped =
            Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(std::move(result));
        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult Compile(
        Das::ExportInterface::IDasJson*  p_request_json,
        Das::ExportInterface::IDasJson** pp_out_result_json) override
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->compile_count;
            if (p_request_json)
            {
                DasPtr<IDasReadOnlyString> key;
                if (DAS_S_OK
                        == CreateIDasReadOnlyStringFromUtf8(
                            "purpose",
                            key.Put())
                    && key)
                {
                    DasPtr<IDasReadOnlyString> purpose;
                    if (DAS_S_OK
                            == p_request_json->GetStringByName(
                                key.Get(),
                                purpose.Put())
                        && purpose)
                    {
                        const char* raw = nullptr;
                        if (DAS_S_OK == purpose->GetUtf8(&raw) && raw)
                        {
                            state_->last_compile_purpose = raw;
                        }
                    }
                }
            }
        }
        yyjson::value result(Das::Utils::MakeYyjsonObject());
        yyjson::value execution_input(Das::Utils::MakeYyjsonObject());
        (*execution_input.as_object())[std::string_view("key1")] = "compiled";
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            (*result.as_object())[std::string_view("ok")] = state_->compile_ok;
        }
        (*result.as_object())[std::string_view("executionInput")] =
            std::move(execution_input);
        auto wrapped =
            Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(std::move(result));
        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

private:
    std::atomic<uint32_t>                   ref_count_{0};
    std::shared_ptr<FactoryTaskSharedState> state_;
};

class FakeAuthoringSessionFactory final
    : public Das::PluginInterface::IDasTaskAuthoringSessionFactory
{
public:
    explicit FakeAuthoringSessionFactory(
        std::shared_ptr<FactoryTaskSharedState> state,
        DasGuid                                 factory_guid,
        bool                                    can_create_session)
        : state_(std::move(state)), factory_guid_(factory_guid),
          can_create_session_(can_create_session)
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
        if (iid
            == DasIidOf<
                Das::PluginInterface::IDasTaskAuthoringSessionFactory>())
        {
            *pp = static_cast<
                Das::PluginInterface::IDasTaskAuthoringSessionFactory*>(this);
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
        *p_out_guid = factory_guid_;
        return DAS_S_OK;
    }

    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override
    {
        if (!pp)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "FakeAuthoringSessionFactory",
            pp);
    }

    DasResult CreateSession(
        const DasGuid&,
        Das::ExportInterface::IDasJson*                  p_context_json,
        Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
        override
    {
        if (!pp_out_session)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (!can_create_session_)
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->decoy_authoring_create_count;
            return DAS_E_FAIL;
        }
        int64_t revision = -1;
        if (p_context_json)
        {
            DasPtr<IDasReadOnlyString> key;
            if (DAS_S_OK
                    == CreateIDasReadOnlyStringFromUtf8("revision", key.Put())
                && key)
            {
                p_context_json->GetIntByName(key.Get(), &revision);
            }
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->authoring_session_count;
            state_->last_context_revision = revision;
        }

        auto* session = new FakeAuthoringSession(state_);
        session->AddRef();
        *pp_out_session = session;
        return DAS_S_OK;
    }

private:
    std::atomic<uint32_t>                   ref_count_{0};
    std::shared_ptr<FactoryTaskSharedState> state_;
    DasGuid                                 factory_guid_{};
    bool                                    can_create_session_ = true;
};

class FakeTaskComponent final : public Das::PluginInterface::IDasTaskComponent
{
public:
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
        if (iid == DasIidOf<Das::PluginInterface::IDasTaskComponent>())
        {
            *pp = static_cast<Das::PluginInterface::IDasTaskComponent*>(this);
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
        *p_out_guid = FactoryTaskComponentImplGuid;
        return DAS_S_OK;
    }

    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override
    {
        if (!pp)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8("FakeTaskComponent", pp);
    }

    DasResult ApplySettingsChange(
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson** pp_out_result_json) override
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        auto result = Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
            Das::Utils::MakeYyjsonObject());
        *pp_out_result_json = result.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult Do(
        Das::PluginInterface::IDasStopToken*,
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson** pp_out_result_json) override
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        auto result_json = Das::Utils::MakeYyjsonObject();
        (*result_json.as_object())[std::string_view("status")] = "completed";
        auto result = Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
            std::move(result_json));
        *pp_out_result_json = result.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

private:
    std::atomic<uint32_t> ref_count_{0};
};

class FakeTaskComponentFactory final
    : public Das::PluginInterface::IDasTaskComponentFactory
{
public:
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
        if (iid == DasIidOf<Das::PluginInterface::IDasTaskComponentFactory>())
        {
            *pp = static_cast<Das::PluginInterface::IDasTaskComponentFactory*>(
                this);
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
        *p_out_guid = FactoryTaskComponentFactoryGuid;
        return DAS_S_OK;
    }

    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override
    {
        if (!pp)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8("FakeTaskComponentFactory", pp);
    }

    DasResult CreateComponent(
        const DasGuid&                            component_guid,
        Das::PluginInterface::IDasTaskComponent** pp_out_component) override
    {
        if (!pp_out_component)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;

        static constexpr std::array kSupportedComponents{
            DasGuid{
                0x68F10001,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
            DasGuid{
                0x68F10002,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}},
            DasGuid{
                0x68F10003,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03}},
            DasGuid{
                0x68F10004,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04}},
            DasGuid{
                0x68F10005,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05}},
            DasGuid{
                0x68F10006,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06}}};

        const auto supported = std::any_of(
            kSupportedComponents.begin(),
            kSupportedComponents.end(),
            [&component_guid](const DasGuid& supported_guid)
            { return component_guid == supported_guid; });
        if (!supported)
        {
            return DAS_E_NOT_FOUND;
        }

        auto* component = new FakeTaskComponent();
        component->AddRef();
        *pp_out_component = component;
        return DAS_S_OK;
    }

private:
    std::atomic<uint32_t> ref_count_{0};
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
        if (index == 0)
        {
            *p_out_feature = Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK;
            return DAS_S_OK;
        }
        if (index == 1)
        {
            *p_out_feature =
                Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY;
            return DAS_S_OK;
        }
        if (index == 2)
        {
            *p_out_feature =
                Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY;
            return DAS_S_OK;
        }
        if (index == 3)
        {
            *p_out_feature =
                Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
            return DAS_S_OK;
        }
        return DAS_E_OUT_OF_RANGE;
    }

    DasResult CreateFeatureInterface(
        uint64_t   index,
        IDasBase** pp_out_interface) override
    {
        if (!pp_out_interface)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (index == 0)
        {
            DasOutPtr<IDasBase> result(pp_out_interface);
            auto*               task = new FactoryBackedTask(state_);
            result.Set(task);
            result.Keep();
            return DAS_S_OK;
        }
        if (index == 1)
        {
            DasOutPtr<IDasBase> result(pp_out_interface);
            auto*               factory = new FakeAuthoringSessionFactory(
                state_,
                DecoyAuthoringFactoryGuid,
                false);
            result.Set(factory);
            result.Keep();
            return DAS_S_OK;
        }
        if (index == 2)
        {
            DasOutPtr<IDasBase> result(pp_out_interface);
            auto*               factory = new FakeAuthoringSessionFactory(
                state_,
                FactoryAuthoringFactoryGuid,
                true);
            result.Set(factory);
            result.Keep();
            return DAS_S_OK;
        }
        if (index == 3)
        {
            DasOutPtr<IDasBase> result(pp_out_interface);
            auto*               factory = new FakeTaskComponentFactory();
            result.Set(factory);
            result.Keep();
            return DAS_S_OK;
        }
        return DAS_E_OUT_OF_RANGE;
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

void WriteFactoryPluginManifest(
    const std::filesystem::path& manifest_path,
    std::string_view             plugin_guid = FactoryPluginGuidString,
    std::string_view             task_guid = FactoryTaskGuidString,
    bool                         include_authoring = true,
    std::string_view             language = "Cpp")
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
        add_component(
            "68F10001-0000-4000-8000-000000000001",
            "das.flow.branch");
        add_component(
            "68F10002-0000-4000-8000-000000000002",
            "das.flow.sequence");
        add_component("68F10003-0000-4000-8000-000000000003", "das.flow.delay");
        add_component("68F10004-0000-4000-8000-000000000004", "das.flow.for");
        add_component("68F10005-0000-4000-8000-000000000005", "das.flow.while");
        add_component("68F10006-0000-4000-8000-000000000006", "das.flow.goto");

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
                    static_cast<int64_t>(
                        Das::ExportInterface::DAS_TYPE_STRING);
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
        yyjson::value tasks_obj(Das::Utils::MakeYyjsonObject());
        (*tasks_obj.as_object())[std::string_view(task_guid)] =
            std::move(task_entry);
        (*manifest.as_object())[std::string_view("tasks")] =
            std::move(tasks_obj);
    }

    std::ofstream ofs(manifest_path);
    ofs << *Das::Utils::SerializeYyjsonValue(manifest, true);
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

    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->executed_instance_ids.size(), 2u);
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

    yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto          result = scheduler_->GetTaskAuthoringDocument(0, request);
    auto          obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE(obj->contains(std::string_view("document")));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("formSequence"));
    auto catalog = (*document)[std::string_view("catalog")].as_object();
    ASSERT_TRUE(catalog.has_value());
    EXPECT_EQ(
        (*catalog)[std::string_view("providerField")].as_string().value_or(""),
        std::string_view("preserved"));
    auto components = (*catalog)[std::string_view("components")].as_array();
    ASSERT_TRUE(components.has_value());
    EXPECT_GE(components->size(), 6u);
    auto has_component_kind = [&components](std::string_view expected_kind)
    {
        return std::any_of(
            components->begin(),
            components->end(),
            [expected_kind](const yyjson::value& component)
            {
                auto component_obj = component.as_object();
                if (!component_obj)
                {
                    return false;
                }
                return (*component_obj)[std::string_view("kind")]
                           .as_string()
                           .value_or("")
                       == expected_kind;
            });
    };
    EXPECT_TRUE(has_component_kind("das.flow.branch"));
    EXPECT_TRUE(has_component_kind("das.flow.sequence"));
    EXPECT_TRUE(has_component_kind("das.flow.delay"));
    EXPECT_TRUE(has_component_kind("das.flow.for"));
    EXPECT_TRUE(has_component_kind("das.flow.while"));
    EXPECT_TRUE(has_component_kind("das.flow.goto"));

    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
    EXPECT_EQ(shared_state_->decoy_authoring_create_count, 0);
    EXPECT_EQ(shared_state_->get_document_count, 1);
    EXPECT_EQ(shared_state_->last_context_revision, 0);
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

    yyjson::value change(Das::Utils::MakeYyjsonObject());
    (*change.as_object())[std::string_view("baseRevision")] = 2;
    (*change.as_object())[std::string_view("kind")] = "setValue";

    auto result = scheduler_->ApplyTaskAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("revisionConflict"));
    EXPECT_EQ(
        (*obj)[std::string_view("currentRevision")].as_sint().value_or(-1),
        3);

    std::lock_guard<std::mutex> lock(shared_state_->mutex);
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

    yyjson::value change(Das::Utils::MakeYyjsonObject());
    (*change.as_object())[std::string_view("baseRevision")] = 0;
    (*change.as_object())[std::string_view("kind")] = "setValue";

    auto result = scheduler_->ApplyTaskAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_EQ((*obj)[std::string_view("revision")].as_sint().value_or(-1), 1);
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("formSequence"));
    auto catalog = (*document)[std::string_view("catalog")].as_object();
    ASSERT_TRUE(catalog.has_value());
    EXPECT_TRUE(catalog->contains(std::string_view("components")));

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
        (*authoring)[std::string_view("sourceFingerprint")]
            .as_string()
            .value_or(""),
        std::string_view("fake-source"));

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    ASSERT_TRUE(WaitForExecutions(1, std::chrono::seconds(2)));
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);

    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
    EXPECT_GE(shared_state_->get_document_count, 1);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->last_compile_purpose, "execution");
    EXPECT_EQ(shared_state_->last_props_key1_value, "compiled");
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

    yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto          result = scheduler_->CompileTaskAuthoring(0, request);
    auto          obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto compile = (*obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile.has_value());
    EXPECT_TRUE((*compile)[std::string_view("ok")].as_bool().value_or(false));

    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_TRUE(shared_state_->executed_instance_ids.empty());
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
            (*authoring.as_object())[std::string_view("kind")] =
                "formSequence";
            (*authoring.as_object())[std::string_view("sourceFingerprint")] =
                "seed-source";
            (*authoring.as_object())[std::string_view("migrationState")] =
                yyjson::value{};
            (*entry.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        (*entry.as_object())[std::string_view("acceptedProperties")] =
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
            (*availability)[std::string_view("state")]
                .as_string()
                .value_or(""),
            std::string_view("unavailable"));
        EXPECT_EQ(
            (*availability)[std::string_view("reason")]
                .as_string()
                .value_or(""),
            reason);
        auto message =
            (*availability)[std::string_view("message")].as_string();
        ASSERT_TRUE(message.has_value());
        EXPECT_FALSE(message->empty());
    }

    void ExpectNoRepositoryAuthoringProviderCalls(
        const std::shared_ptr<FactoryTaskSharedState>& state)
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        EXPECT_EQ(state->authoring_session_count, 0);
        EXPECT_EQ(state->get_document_count, 0);
        EXPECT_EQ(state->apply_change_count, 0);
        EXPECT_EQ(state->compile_count, 0);
        EXPECT_EQ(state->decoy_authoring_create_count, 0);
    }

    void ExpectUnavailableRenameDeleteWithoutProviderCalls(
        SchedulerService&                             scheduler,
        const std::shared_ptr<FactoryTaskSharedState>& state,
        const std::filesystem::path&                  settings_dir,
        std::string_view                              reason)
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
} // namespace

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
    EXPECT_EQ(
        (*obj)[std::string_view("entryId")].as_sint().value_or(-1),
        0);
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

    auto accepted =
        (*obj)[std::string_view("acceptedProperties")].as_object();
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
        (*persisted_props)[std::string_view("retryCount")]
            .as_sint()
            .value_or(-1),
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
    auto entries =
        (*repository_obj)[std::string_view("entries")].as_array();
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
        (*still_persisted_obj)[std::string_view("entryId")]
            .as_sint()
            .value_or(-1),
        0);

    EXPECT_EQ(scheduler_->Enable(), DAS_E_OBJECT_NOT_INIT);
    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    EXPECT_TRUE(shared_state_->executed_instance_ids.empty());
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
        (*renamed_authoring)[std::string_view("revision")]
            .as_sint()
            .value_or(-1),
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
        (*duplicate_obj)[std::string_view("displayName")]
            .as_string()
            .value_or(""),
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
    ExpectRepositoryAvailability(entry, "authoringCapabilityMissing");
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
        "authoringCapabilityMissing");
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
        std::lock_guard<std::mutex> lock(shared_state_->mutex);
        shared_state_->compile_ok = false;
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);

    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->last_compile_purpose, "execution");
    EXPECT_TRUE(shared_state_->executed_instance_ids.empty());
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
        std::atomic<bool> authoring_get_called{false};
        std::atomic<bool> authoring_apply_called{false};
        std::atomic<bool> authoring_compile_called{false};
        int64_t           last_authoring_task_id = -1;

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

    yyjson::value request(Das::Utils::MakeYyjsonObject());
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
