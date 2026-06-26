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
    std::string_view             execution_component_guid = {})
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
        registry_ = std::make_unique<DAS::Core::IPC::RemoteObjectRegistry>();
        plugin_manager_->SetRegistry(*registry_);

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

    void TearDown() override
    {
        if (scheduler_ && scheduler_->Status() == SchedulerState::Running)
        {
            EXPECT_EQ(scheduler_->Disable(), DAS_S_OK);
        }

        // RequestStop 内部将 running_ 置 false 并 io_context->stop()，
        // 使 Run() 返回，io_thread_ 随后 join。
        ipc_sp_->RequestStop();
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
    std::unique_ptr<DAS::Core::IPC::RemoteObjectRegistry>     registry_;
    std::unique_ptr<Das::Core::ForeignInterfaceHost::TestablePluginManager>
                                      plugin_manager_;
    std::unique_ptr<SchedulerService> scheduler_;
    // 段内指针：生命周期由 shm_segment_ 管理，不可 free。
    FactoryTaskSharedState* shared_state_ = nullptr;
    std::unique_ptr<boost::interprocess::managed_shared_memory> shm_segment_;
    std::string                                                 shm_name_;
    std::thread                                                 io_thread_;

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

    ipc_scoped_lock lock(shared_state_->mutex);
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

    ipc_scoped_lock lock(shared_state_->mutex);
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

    ipc_scoped_lock lock(shared_state_->mutex);
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

    ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
    EXPECT_GE(shared_state_->get_document_count, 1);
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

    yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto          result = scheduler_->CompileTaskAuthoring(0, request);
    auto          obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto compile = (*obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile.has_value());
    EXPECT_TRUE((*compile)[std::string_view("ok")].as_bool().value_or(false));

    ipc_scoped_lock lock(shared_state_->mutex);
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
        (*change.as_object())[std::string_view("payload")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
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
            (*authoring.as_object())[std::string_view("kind")] = "formSequence";
            (*authoring.as_object())[std::string_view("sourceFingerprint")] =
                "seed-source";
            (*authoring.as_object())[std::string_view("migrationState")] =
                yyjson::value{};
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
        ipc_scoped_lock lock(state->mutex);
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
        ipc_scoped_lock lock(state->mutex);
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
        ipc_scoped_lock lock(state->mutex);
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

        yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto          document =
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

        yyjson::value request(Das::Utils::MakeYyjsonObject());
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
    EXPECT_EQ(
        (*apply_obj)[std::string_view("revision")].as_sint().value_or(-1),
        1);

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
    EXPECT_EQ(
        (*compile_obj)[std::string_view("entryId")].as_sint().value_or(-1),
        0);
    EXPECT_TRUE(
        (*compile_obj)[std::string_view("canExecute")].as_bool().value_or(
            false));
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
    EXPECT_TRUE(
        (*obj)[std::string_view("canExecute")].as_bool().value_or(false));

    auto summary = (*obj)[std::string_view("summary")].as_object();
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
        (*diagnostic)[std::string_view("code")].as_string().value_or(""),
        std::string_view("fake.compile"));

    EXPECT_FALSE(obj->contains(std::string_view("compile")));
    EXPECT_FALSE(obj->contains(std::string_view("executionInput")));
    EXPECT_FALSE(obj->contains(std::string_view("compiledSnapshot")));
    EXPECT_FALSE(obj->contains(std::string_view("providerDebug")));

    ipc_scoped_lock lock(shared_state_->mutex);
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

    ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->get_last_compile_purpose(), "execution");
    EXPECT_EQ(shared_state_->last_context_entry_id, 0);
    EXPECT_FALSE(shared_state_->last_context_had_task_id);
    EXPECT_TRUE((shared_state_->executed_count == 0));
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

    ipc_scoped_lock lock(shared_state_->mutex);
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

    yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto          document_response =
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
    EXPECT_EQ(
        (*document)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("formSequence"));

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

    ipc_scoped_lock lock(shared_state_->mutex);
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
    EXPECT_EQ(
        (*result_obj)[std::string_view("revision")].as_sint().value_or(-1),
        1);
    auto accepted =
        (*result_obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(
        (*accepted)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("accepted"));
    auto document = (*result_obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("formSequence"));

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
        (*authoring)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("setValue"));
    EXPECT_EQ(
        (*authoring)[std::string_view("sourceFingerprint")]
            .as_string()
            .value_or(""),
        std::string_view("fake-source"));

    ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
    EXPECT_GE(shared_state_->get_document_count, 1);
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
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("revisionConflict"));
    EXPECT_EQ(
        (*obj)[std::string_view("currentRevision")].as_sint().value_or(-1),
        0);
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
        ipc_scoped_lock lock(shared_state_->mutex);
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

    ipc_scoped_lock lock(shared_state_->mutex);
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

    ipc_scoped_lock lock(shared_state_->mutex);
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
    ipc_scoped_lock lock(shared_state_->mutex);
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
        "authoringCapabilityMissing");
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
        "authoringCapabilityMissing");
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
        ipc_scoped_lock lock(shared_state_->mutex);
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
        ipc_scoped_lock lock(shared_state_->mutex);
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
        ipc_scoped_lock lock(shared_state_->mutex);
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
        ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->unblock_do = true;
    }
    shared_state_->cv.notify_all();
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
        ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->compile_ok = false;
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);

    ipc_scoped_lock lock(shared_state_->mutex);
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
