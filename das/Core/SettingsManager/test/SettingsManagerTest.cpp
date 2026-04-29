#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // Use a stable base directory under the build output to avoid
    // temp_directory_path() issues in some test environments.
    std::filesystem::path GetTestBaseDir()
    {
        // Prefer DAS_TEST_TMPDIR if set, otherwise use current working
        // directory
        const char* env = std::getenv("DAS_TEST_TMPDIR");
        if (env && env[0] != '\0')
        {
            return std::filesystem::path(env);
        }
        return std::filesystem::current_path();
    }

    class SettingsManagerTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            test_dir_ = GetTestBaseDir()
                        / ("das_test_"
                           + std::to_string(
                               std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
            std::filesystem::create_directories(test_dir_);
        }

        void TearDown() override { std::filesystem::remove_all(test_dir_); }

        std::filesystem::path test_dir_;
    };

    TEST_F(SettingsManagerTest, GetGlobalSettings_ReturnsEmptyObject_WhenNoFile)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        auto result = sm.GetGlobalSettings();
        EXPECT_EQ(result, "{}");
    }

    TEST_F(SettingsManagerTest, UpdateGlobalSettings_PersistsToFile)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        auto data = Das::Utils::MakeYyjsonObject();
        {
            auto obj = *data.as_object();
            obj[std::string_view("theme")] = "dark";
            obj[std::string_view("language")] = "zh-CN";
        }

        auto update_result = sm.UpdateGlobalSettings(
            *Das::Utils::SerializeYyjsonValue(data, false));
        EXPECT_EQ(update_result, DAS_S_OK);

        // Create a new instance from the same directory to verify persistence
        Das::Core::SettingsManager::SettingsManager sm2(test_dir_);
        auto loaded = sm2.GetGlobalSettings();
        auto loaded_json_opt = Das::Utils::ParseYyjsonFromString(loaded);
        ASSERT_TRUE(loaded_json_opt.has_value());
        auto& loaded_json = *loaded_json_opt;
        EXPECT_EQ(
            std::string((*loaded_json.as_object())[std::string_view("theme")]
                            .as_string()
                            .value()),
            "dark");
        EXPECT_EQ(
            std::string((*loaded_json.as_object())[std::string_view("language")]
                            .as_string()
                            .value()),
            "zh-CN");
    }

    TEST_F(SettingsManagerTest, CreateProfile_CreatesDirectory)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        auto result = sm.CreateProfile("0");
        EXPECT_EQ(result, DAS_S_OK);

        auto profile_dir = test_dir_ / "0";
        EXPECT_TRUE(std::filesystem::exists(profile_dir));
        EXPECT_TRUE(std::filesystem::is_directory(profile_dir));
    }

    TEST_F(SettingsManagerTest, GetProfileList_ReturnsCreatedProfiles)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        auto result = sm.GetProfileList();
        auto profiles_opt = Das::Utils::ParseYyjsonFromString(result);
        ASSERT_TRUE(profiles_opt.has_value());
        auto& profiles = *profiles_opt;
        ASSERT_TRUE(profiles.is_array());
        ASSERT_EQ(profiles.as_array()->size(), 1u);
        EXPECT_EQ(
            std::string((*(*profiles.as_array())[0]
                              .as_object())[std::string_view("profileId")]
                            .as_string()
                            .value()),
            "0");
    }

    TEST_F(SettingsManagerTest, DeleteProfile_RemovesDirectory)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        auto result = sm.DeleteProfile("0");
        EXPECT_EQ(result, DAS_S_OK);

        auto profile_dir = test_dir_ / "0";
        EXPECT_FALSE(std::filesystem::exists(profile_dir));
    }

    TEST_F(SettingsManagerTest, GetProfile_ReturnsData_AfterUpdate)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value profile_data(
            Das::Utils::MakeYyjsonObject());
        (*profile_data.as_object())[std::string_view("name")] = "test-profile";
        (*profile_data.as_object())[std::string_view("active")] = true;
        auto update_result = sm.UpdateProfile(
            "0",
            *Das::Utils::SerializeYyjsonValue(profile_data, false));
        EXPECT_EQ(update_result, DAS_S_OK);

        auto result = sm.GetProfile("0");
        auto loaded_opt = Das::Utils::ParseYyjsonFromString(result);
        ASSERT_TRUE(loaded_opt.has_value());
        auto& loaded = *loaded_opt;
        EXPECT_EQ(
            std::string((*loaded.as_object())[std::string_view("name")]
                            .as_string()
                            .value()),
            "test-profile");
        EXPECT_EQ(
            (*loaded.as_object())[std::string_view("active")].as_bool().value(),
            true);
    }

    TEST_F(SettingsManagerTest, GetPluginSettings_ReturnsData_AfterUpdate)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value plugin_data(
            Das::Utils::MakeYyjsonObject());
        (*plugin_data.as_object())[std::string_view("setting1")] = "value1";
        (*plugin_data.as_object())[std::string_view("count")] = 42;
        auto update_result = sm.UpdatePluginSettings(
            "0",
            "test-guid",
            *Das::Utils::SerializeYyjsonValue(plugin_data, false));
        EXPECT_EQ(update_result, DAS_S_OK);

        auto result = sm.GetPluginSettings("0", "test-guid");
        auto loaded_opt = Das::Utils::ParseYyjsonFromString(result);
        ASSERT_TRUE(loaded_opt.has_value());
        auto& loaded = *loaded_opt;
        EXPECT_EQ(
            std::string((*loaded.as_object())[std::string_view("setting1")]
                            .as_string()
                            .value()),
            "value1");
        EXPECT_EQ(
            (*loaded.as_object())[std::string_view("count")].as_sint().value(),
            42);
    }

    TEST_F(SettingsManagerTest, ConcurrentReadWrite_NoCorruption)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        constexpr int num_iterations = 50;
        constexpr int num_writers = 2;
        constexpr int num_readers = 2;

        std::vector<std::thread> threads;

        // Writer threads
        for (int w = 0; w < num_writers; ++w)
        {
            threads.emplace_back(
                [&sm, w]()
                {
                    for (int i = 0; i < num_iterations; ++i)
                    {
                        yyjson::writer::detail::value data(
                            Das::Utils::MakeYyjsonObject());
                        (*data.as_object())[std::string_view("writer")] = w;
                        (*data.as_object())[std::string_view("iteration")] = i;
                        sm.UpdateGlobalSettings(
                            *Das::Utils::SerializeYyjsonValue(data, false));
                    }
                });
        }

        // Reader threads
        for (int r = 0; r < num_readers; ++r)
        {
            threads.emplace_back(
                [&sm]()
                {
                    for (int i = 0; i < num_iterations; ++i)
                    {
                        auto result = sm.GetGlobalSettings();
                        // Result must be valid JSON
                        auto parsed_opt =
                            Das::Utils::ParseYyjsonFromString(result);
                        ASSERT_TRUE(parsed_opt.has_value());
                        EXPECT_TRUE(parsed_opt->is_object());
                    }
                });
        }

        for (auto& t : threads)
        {
            t.join();
        }

        // Verify final state is valid JSON
        auto final_result = sm.GetGlobalSettings();
        auto final_json_opt = Das::Utils::ParseYyjsonFromString(final_result);
        ASSERT_TRUE(final_json_opt.has_value());
        auto& final_json = *final_json_opt;
        EXPECT_TRUE(final_json.is_object());
        EXPECT_FALSE(
            (*final_json.as_object())[std::string_view("writer")].is_null());
        EXPECT_FALSE(
            (*final_json.as_object())[std::string_view("iteration")].is_null());
    }

    // --- Split-file plugin settings tests ---

    TEST_F(SettingsManagerTest, PluginSettings_UsesSplitFile)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value plugin_data(
            Das::Utils::MakeYyjsonObject());
        (*plugin_data.as_object())[std::string_view("adbPath")] =
            "/usr/bin/adb";
        (*plugin_data.as_object())[std::string_view("timeout")] = 30;
        auto result =
            sm.UpdatePluginSettingsJson("0", "plugin-guid-1", plugin_data);
        EXPECT_EQ(result, DAS_S_OK);

        // Verify a separate file was created
        auto guid_file = test_dir_ / "0" / "plugin-guid-1.json";
        EXPECT_TRUE(std::filesystem::exists(guid_file));

        // Read back through GetPluginSettingsJson
        auto loaded = sm.GetPluginSettingsJson("0", "plugin-guid-1");
        EXPECT_EQ(
            std::string((*loaded.as_object())[std::string_view("adbPath")]
                            .as_string()
                            .value()),
            "/usr/bin/adb");
        EXPECT_EQ(
            (*loaded.as_object())[std::string_view("timeout")]
                .as_sint()
                .value(),
            30);
    }

    TEST_F(SettingsManagerTest, PluginSettings_NotInUiJson)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value plugin_data(
            Das::Utils::MakeYyjsonObject());
        (*plugin_data.as_object())[std::string_view("key")] = "value";
        auto result =
            sm.UpdatePluginSettingsJson("0", "some-guid", plugin_data);
        EXPECT_EQ(result, DAS_S_OK);

        // ui.json should not contain plugin GUID data
        auto profile = sm.GetProfileJson("0");
        EXPECT_FALSE(
            (*profile.as_object()).contains(std::string_view("some-guid")));
    }

    TEST_F(SettingsManagerTest, PluginSettings_FieldUpdate)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Set initial plugin settings
        yyjson::writer::detail::value plugin_data(
            Das::Utils::MakeYyjsonObject());
        (*plugin_data.as_object())[std::string_view("field1")] = "original";
        (*plugin_data.as_object())[std::string_view("field2")] = 100;
        sm.UpdatePluginSettingsJson("0", "my-guid", plugin_data);

        // Update a single field
        {
            yyjson::writer::detail::value field_val("updated");
            auto result = sm.UpdatePluginSettingsFieldJson(
                "0",
                "my-guid",
                "field1",
                field_val);
            EXPECT_EQ(result, DAS_S_OK);
        }

        // Verify field was updated
        auto field = sm.GetPluginSettingsFieldJson("0", "my-guid", "field1");
        EXPECT_EQ(std::string(field.as_string().value()), "updated");

        // Verify other field unchanged
        auto field2 = sm.GetPluginSettingsFieldJson("0", "my-guid", "field2");
        EXPECT_EQ(field2.as_sint().value(), 100);
    }

    TEST_F(SettingsManagerTest, PluginSettings_PersistsAcrossInstances)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value plugin_data(
            Das::Utils::MakeYyjsonObject());
        (*plugin_data.as_object())[std::string_view("setting")] = "persistent";
        sm.UpdatePluginSettingsJson("0", "guid-123", plugin_data);

        // New instance should read the same data
        Das::Core::SettingsManager::SettingsManager sm2(test_dir_);
        auto loaded = sm2.GetPluginSettingsJson("0", "guid-123");
        EXPECT_EQ(
            std::string((*loaded.as_object())[std::string_view("setting")]
                            .as_string()
                            .value()),
            "persistent");
    }

    // --- Scheduler index tests (scheduler.json) ---

    TEST_F(SettingsManagerTest, SchedulerIndex_ReturnsDefault_WhenMissing)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        auto state = sm.GetSchedulerIndexJson("0");
        EXPECT_EQ(
            (*state.as_object())[std::string_view("nextTaskId")]
                .as_sint()
                .value(),
            0);
        EXPECT_TRUE(
            (*state.as_object())[std::string_view("taskOrder")].is_array());
        EXPECT_EQ(
            (*state.as_object())[std::string_view("taskOrder")]
                .as_array()
                ->size(),
            0u);
    }

    TEST_F(SettingsManagerTest, SchedulerIndex_PersistsNextTaskId)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value state(Das::Utils::MakeYyjsonObject());
        (*state.as_object())[std::string_view("nextTaskId")] = 5;
        (*state.as_object())[std::string_view("taskOrder")] =
            yyjson::writer::detail::value(Das::Utils::MakeYyjsonArray());
        auto result = sm.UpdateSchedulerIndexJson("0", state);
        EXPECT_EQ(result, DAS_S_OK);

        // Verify separate file created
        auto scheduler_file = test_dir_ / "0" / "scheduler.json";
        EXPECT_TRUE(std::filesystem::exists(scheduler_file));

        // Read back
        auto loaded = sm.GetSchedulerIndexJson("0");
        EXPECT_EQ(
            (*loaded.as_object())[std::string_view("nextTaskId")]
                .as_sint()
                .value(),
            5);
        EXPECT_TRUE(
            (*loaded.as_object())[std::string_view("taskOrder")].is_array());
    }

    TEST_F(SettingsManagerTest, SchedulerIndex_PersistsAcrossInstances)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value state(Das::Utils::MakeYyjsonObject());
        (*state.as_object())[std::string_view("nextTaskId")] = 3;
        {
            yyjson::writer::detail::value arr(Das::Utils::MakeYyjsonArray());
            (*arr.as_array()).emplace_back(0);
            (*arr.as_array()).emplace_back(1);
            (*state.as_object())[std::string_view("taskOrder")] =
                std::move(arr);
        }
        sm.UpdateSchedulerIndexJson("0", state);

        // New instance reads the same state
        Das::Core::SettingsManager::SettingsManager sm2(test_dir_);
        auto loaded = sm2.GetSchedulerIndexJson("0");
        EXPECT_EQ(
            (*loaded.as_object())[std::string_view("nextTaskId")]
                .as_sint()
                .value(),
            3);
        ASSERT_EQ(
            (*loaded.as_object())[std::string_view("taskOrder")]
                .as_array()
                ->size(),
            2u);
        EXPECT_EQ(
            (*(*loaded.as_object())[std::string_view("taskOrder")]
                  .as_array())[0]
                .as_sint()
                .value(),
            0);
        EXPECT_EQ(
            (*(*loaded.as_object())[std::string_view("taskOrder")]
                  .as_array())[1]
                .as_sint()
                .value(),
            1);
    }

    TEST_F(SettingsManagerTest, SchedulerIndex_OrderedTaskIdsPreserved)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value state(Das::Utils::MakeYyjsonObject());
        (*state.as_object())[std::string_view("nextTaskId")] = 3;
        {
            yyjson::writer::detail::value arr(Das::Utils::MakeYyjsonArray());
            (*arr.as_array()).emplace_back(2);
            (*arr.as_array()).emplace_back(0);
            (*arr.as_array()).emplace_back(1);
            (*state.as_object())[std::string_view("taskOrder")] =
                std::move(arr);
        }
        sm.UpdateSchedulerIndexJson("0", state);

        auto loaded = sm.GetSchedulerIndexJson("0");
        ASSERT_EQ(
            (*loaded.as_object())[std::string_view("taskOrder")]
                .as_array()
                ->size(),
            3u);
        EXPECT_EQ(
            (*(*loaded.as_object())[std::string_view("taskOrder")]
                  .as_array())[0]
                .as_sint()
                .value(),
            2);
        EXPECT_EQ(
            (*(*loaded.as_object())[std::string_view("taskOrder")]
                  .as_array())[1]
                .as_sint()
                .value(),
            0);
        EXPECT_EQ(
            (*(*loaded.as_object())[std::string_view("taskOrder")]
                  .as_array())[2]
                .as_sint()
                .value(),
            1);
    }

    TEST_F(SettingsManagerTest, SchedulerIndex_NotInUiJson)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value state(Das::Utils::MakeYyjsonObject());
        (*state.as_object())[std::string_view("nextTaskId")] = 1;
        (*state.as_object())[std::string_view("taskOrder")] =
            yyjson::writer::detail::value(Das::Utils::MakeYyjsonArray());
        sm.UpdateSchedulerIndexJson("0", state);

        // ui.json should not contain scheduler data
        auto profile = sm.GetProfileJson("0");
        EXPECT_FALSE(
            (*profile.as_object()).contains(std::string_view("scheduler")));
    }

    // --- Task instance tests (taskId{id}.json) ---

    TEST_F(SettingsManagerTest, TaskInstance_ReadWrite)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value task(Das::Utils::MakeYyjsonObject());
        (*task.as_object())[std::string_view("id")] = 0;
        (*task.as_object())[std::string_view("taskGuid")] =
            "B4F60C54-67DF-407A-B891-6D3C90CDB9A1";
        (*task.as_object())[std::string_view("pluginGuid")] =
            "8F08935F-11B9-4D3A-BB0D-96D1862FE3F6";
        (*task.as_object())[std::string_view("nextExecutionTime")] =
            yyjson::writer::detail::value{};
        {
            yyjson::writer::detail::value props(Das::Utils::MakeYyjsonObject());
            (*props.as_object())[std::string_view("claimMail")] = true;
            (*props.as_object())[std::string_view("maxRetryCount")] = 3;
            (*task.as_object())[std::string_view("properties")] =
                std::move(props);
        }

        auto result = sm.UpdateTaskInstanceJson("0", 0, task);
        EXPECT_EQ(result, DAS_S_OK);

        // Verify file was created with expected name
        auto task_file = test_dir_ / "0" / "taskId0.json";
        EXPECT_TRUE(std::filesystem::exists(task_file));

        auto loaded = sm.GetTaskInstanceJson("0", 0);
        EXPECT_EQ(
            (*loaded.as_object())[std::string_view("id")].as_sint().value(),
            0);
        EXPECT_EQ(
            std::string((*loaded.as_object())[std::string_view("taskGuid")]
                            .as_string()
                            .value()),
            "B4F60C54-67DF-407A-B891-6D3C90CDB9A1");
        EXPECT_EQ(
            (*(*loaded.as_object())[std::string_view("properties")]
                  .as_object())[std::string_view("claimMail")]
                .as_bool()
                .value(),
            true);
        EXPECT_EQ(
            (*(*loaded.as_object())[std::string_view("properties")]
                  .as_object())[std::string_view("maxRetryCount")]
                .as_sint()
                .value(),
            3);
    }

    TEST_F(SettingsManagerTest, TaskInstance_DeleteRemovesFile)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value task(Das::Utils::MakeYyjsonObject());
        (*task.as_object())[std::string_view("id")] = 5;
        sm.UpdateTaskInstanceJson("0", 5, task);

        auto task_file = test_dir_ / "0" / "taskId5.json";
        EXPECT_TRUE(std::filesystem::exists(task_file));

        auto result = sm.DeleteTaskInstanceJson("0", 5);
        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_FALSE(std::filesystem::exists(task_file));
    }

    TEST_F(SettingsManagerTest, TaskInstance_DeleteNonexistent_ReturnsFalse)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        auto result = sm.DeleteTaskInstanceJson("0", 99);
        EXPECT_EQ(result, DAS_S_FALSE);
    }

    TEST_F(SettingsManagerTest, TaskInstance_PersistsAcrossInstances)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value task(Das::Utils::MakeYyjsonObject());
        (*task.as_object())[std::string_view("id")] = 0;
        (*task.as_object())[std::string_view("taskGuid")] =
            "B4F60C54-67DF-407A-B891-6D3C90CDB9A1";
        {
            yyjson::writer::detail::value props(Das::Utils::MakeYyjsonObject());
            (*props.as_object())[std::string_view("claimMail")] = false;
            (*task.as_object())[std::string_view("properties")] =
                std::move(props);
        }
        sm.UpdateTaskInstanceJson("0", 0, task);

        Das::Core::SettingsManager::SettingsManager sm2(test_dir_);
        auto loaded = sm2.GetTaskInstanceJson("0", 0);
        EXPECT_EQ(
            (*loaded.as_object())[std::string_view("id")].as_sint().value(),
            0);
        EXPECT_EQ(
            (*(*loaded.as_object())[std::string_view("properties")]
                  .as_object())[std::string_view("claimMail")]
                .as_bool()
                .value(),
            false);
    }

    // --- Fault isolation tests ---

    TEST_F(
        SettingsManagerTest,
        CorruptPluginSettings_RebuildFromDefaults_ReturnsSFalse)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Write corrupt JSON to plugin settings file
        auto guid_file = test_dir_ / "0" / "corrupt-guid.json";
        {
            std::ofstream ofs{guid_file};
            ofs << "{not valid json!!!";
        }

        // Rebuild should restore defaults and return DAS_S_FALSE
        std::vector<std::string> names = {"adbPath", "timeout"};
        std::vector<std::string> defaults = {"\"/usr/bin/adb\"", "30"};
        auto                     result = sm.RebuildPluginSettingsFromDefaults(
            "0",
            "corrupt-guid",
            names,
            defaults);
        EXPECT_EQ(result, DAS_S_FALSE);

        // File should now contain the defaults
        auto loaded = sm.GetPluginSettingsJson("0", "corrupt-guid");
        EXPECT_EQ(
            std::string((*loaded.as_object())[std::string_view("adbPath")]
                            .as_string()
                            .value()),
            "/usr/bin/adb");
        EXPECT_EQ(
            (*loaded.as_object())[std::string_view("timeout")]
                .as_sint()
                .value(),
            30);
    }

    TEST_F(
        SettingsManagerTest,
        CorruptPluginSettings_WithExistingFile_RebuildFromDefaults)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Write valid JSON first
        yyjson::writer::detail::value valid(Das::Utils::MakeYyjsonObject());
        (*valid.as_object())[std::string_view("existingKey")] = "existingValue";
        sm.UpdatePluginSettingsJson("0", "good-guid", valid);

        // Rebuild on a valid file should return DAS_S_OK (no rebuild needed)
        std::vector<std::string> names = {"defaultKey"};
        std::vector<std::string> defaults = {"\"defaultValue\""};
        auto                     result = sm.RebuildPluginSettingsFromDefaults(
            "0",
            "good-guid",
            names,
            defaults);
        EXPECT_EQ(result, DAS_S_OK);

        // Existing data should be preserved (not overwritten with defaults)
        auto loaded = sm.GetPluginSettingsJson("0", "good-guid");
        EXPECT_EQ(
            std::string((*loaded.as_object())[std::string_view("existingKey")]
                            .as_string()
                            .value()),
            "existingValue");
    }

    TEST_F(SettingsManagerTest, CorruptTaskInstance_DoesNotCorruptOtherFiles)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Create two task instance files
        yyjson::writer::detail::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] = "guid-A";
        sm.UpdateTaskInstanceJson("0", 0, task0);

        yyjson::writer::detail::value task1(Das::Utils::MakeYyjsonObject());
        (*task1.as_object())[std::string_view("id")] = 1;
        (*task1.as_object())[std::string_view("taskGuid")] = "guid-B";
        sm.UpdateTaskInstanceJson("0", 1, task1);

        // Corrupt task0's file
        auto task0_file = test_dir_ / "0" / "taskId0.json";
        {
            std::ofstream ofs{task0_file};
            ofs << "CORRUPT DATA {{{";
        }

        // task1 should still be readable
        auto loaded1 = sm.GetTaskInstanceJson("0", 1);
        EXPECT_EQ(
            (*loaded1.as_object())[std::string_view("id")].as_sint().value(),
            1);
        EXPECT_EQ(
            std::string((*loaded1.as_object())[std::string_view("taskGuid")]
                            .as_string()
                            .value()),
            "guid-B");

        // ui.json should still be intact
        auto profile = sm.GetProfileJson("0");
        EXPECT_TRUE(profile.is_object());

        // scheduler.json should still be independent
        auto scheduler = sm.GetSchedulerIndexJson("0");
        EXPECT_EQ(
            (*scheduler.as_object())[std::string_view("nextTaskId")]
                .as_sint()
                .value(),
            0);
    }

    TEST_F(SettingsManagerTest, MissingPluginSettingsFile_RebuildFromDefaults)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // No file exists for this GUID
        auto guid_file = test_dir_ / "0" / "missing-guid.json";
        EXPECT_FALSE(std::filesystem::exists(guid_file));

        // Rebuild should create the file and return DAS_S_FALSE
        std::vector<std::string> names = {"key1"};
        std::vector<std::string> defaults = {"\"value1\""};
        auto                     result = sm.RebuildPluginSettingsFromDefaults(
            "0",
            "missing-guid",
            names,
            defaults);
        EXPECT_EQ(result, DAS_S_FALSE);

        auto loaded = sm.GetPluginSettingsJson("0", "missing-guid");
        EXPECT_EQ(
            std::string((*loaded.as_object())[std::string_view("key1")]
                            .as_string()
                            .value()),
            "value1");
    }

    // --- Coexistence test ---

    TEST_F(SettingsManagerTest, AllSplitFilesCoexist_Independently)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Write to ui.json
        {
            yyjson::writer::detail::value ui_data(
                Das::Utils::MakeYyjsonObject());
            (*ui_data.as_object())[std::string_view("theme")] = "dark";
            sm.UpdateProfileJson("0", ui_data);
        }

        // Write plugin settings
        {
            yyjson::writer::detail::value plugin_data(
                Das::Utils::MakeYyjsonObject());
            (*plugin_data.as_object())[std::string_view("adbPath")] =
                "/usr/bin/adb";
            sm.UpdatePluginSettingsJson(
                "0",
                "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E",
                plugin_data);
        }

        // Write scheduler index
        {
            yyjson::writer::detail::value scheduler(
                Das::Utils::MakeYyjsonObject());
            (*scheduler.as_object())[std::string_view("nextTaskId")] = 1;
            {
                yyjson::writer::detail::value arr(
                    Das::Utils::MakeYyjsonArray());
                (*arr.as_array()).emplace_back(0);
                (*scheduler.as_object())[std::string_view("taskOrder")] =
                    std::move(arr);
            }
            sm.UpdateSchedulerIndexJson("0", scheduler);
        }

        // Write task instance
        {
            yyjson::writer::detail::value task(Das::Utils::MakeYyjsonObject());
            (*task.as_object())[std::string_view("id")] = 0;
            (*task.as_object())[std::string_view("taskGuid")] =
                "B4F60C54-67DF-407A-B891-6D3C90CDB9A1";
            sm.UpdateTaskInstanceJson("0", 0, task);
        }

        // All files exist independently
        EXPECT_TRUE(std::filesystem::exists(test_dir_ / "0" / "ui.json"));
        EXPECT_TRUE(
            std::filesystem::exists(
                test_dir_ / "0" / "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E.json"));
        EXPECT_TRUE(
            std::filesystem::exists(test_dir_ / "0" / "scheduler.json"));
        EXPECT_TRUE(std::filesystem::exists(test_dir_ / "0" / "taskId0.json"));

        // Each reads back correctly
        EXPECT_EQ(
            std::string(
                (*sm.GetProfileJson("0").as_object())[std::string_view("theme")]
                    .as_string()
                    .value()),
            "dark");
        EXPECT_EQ(
            std::string((*sm.GetPluginSettingsJson(
                                "0",
                                "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E")
                              .as_object())[std::string_view("adbPath")]
                            .as_string()
                            .value()),
            "/usr/bin/adb");
        EXPECT_EQ(
            (*sm.GetSchedulerIndexJson("0")
                  .as_object())[std::string_view("nextTaskId")]
                .as_sint()
                .value(),
            1);
        EXPECT_EQ(
            std::string((*sm.GetTaskInstanceJson("0", 0)
                              .as_object())[std::string_view("taskGuid")]
                            .as_string()
                            .value()),
            "B4F60C54-67DF-407A-B891-6D3C90CDB9A1");
    }

    // --- GetPluginSettingsWithStatus tests ---

    TEST_F(SettingsManagerTest, GetPluginSettingsWithStatus_ValidFile_ReturnsOK)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        yyjson::writer::detail::value plugin_data(
            Das::Utils::MakeYyjsonObject());
        (*plugin_data.as_object())[std::string_view("setting1")] = "value1";
        sm.UpdatePluginSettingsJson("0", "test-guid", plugin_data);

        auto [json, status] = sm.GetPluginSettingsWithStatus("0", "test-guid");
        EXPECT_EQ(status, DAS_S_OK);
        EXPECT_EQ(
            std::string((*json.as_object())[std::string_view("setting1")]
                            .as_string()
                            .value()),
            "value1");
    }

    TEST_F(
        SettingsManagerTest,
        GetPluginSettingsWithStatus_CorruptFile_ReturnsSFalse)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Write corrupt JSON to plugin settings file
        auto guid_file = test_dir_ / "0" / "corrupt-guid.json";
        {
            std::ofstream ofs{guid_file};
            ofs << "{not valid json!!!";
        }

        auto [json, status] =
            sm.GetPluginSettingsWithStatus("0", "corrupt-guid");
        EXPECT_EQ(status, DAS_S_FALSE);
        EXPECT_TRUE(json.is_object());
    }

    TEST_F(
        SettingsManagerTest,
        GetPluginSettingsWithStatus_MissingFile_ReturnsSFalse)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        auto guid_file = test_dir_ / "0" / "missing-guid.json";
        EXPECT_FALSE(std::filesystem::exists(guid_file));

        auto [json, status] =
            sm.GetPluginSettingsWithStatus("0", "missing-guid");
        EXPECT_EQ(status, DAS_S_FALSE);
        EXPECT_TRUE(json.is_object());

        // File should now exist (rebuilt)
        EXPECT_TRUE(std::filesystem::exists(guid_file));
    }

    TEST_F(
        SettingsManagerTest,
        GetPluginSettingsWithStatus_NonObjectJson_ReturnsSFalse)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Write a valid JSON array (not an object) to the plugin settings file
        auto guid_file = test_dir_ / "0" / "array-guid.json";
        {
            std::ofstream ofs{guid_file};
            ofs << "[1, 2, 3]";
        }

        auto [json, status] = sm.GetPluginSettingsWithStatus("0", "array-guid");
        EXPECT_EQ(status, DAS_S_FALSE);
        EXPECT_TRUE(json.is_object());
    }

} // anonymous namespace
