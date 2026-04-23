#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/IDasBase.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
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
        nlohmann::json                              data;
        data["theme"] = "dark";
        data["language"] = "zh-CN";

        auto update_result = sm.UpdateGlobalSettings(data.dump());
        EXPECT_EQ(update_result, DAS_S_OK);

        // Create a new instance from the same directory to verify persistence
        Das::Core::SettingsManager::SettingsManager sm2(test_dir_);
        auto loaded = sm2.GetGlobalSettings();
        auto loaded_json = nlohmann::json::parse(loaded);
        EXPECT_EQ(loaded_json["theme"], "dark");
        EXPECT_EQ(loaded_json["language"], "zh-CN");
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
        auto profiles = nlohmann::json::parse(result);
        ASSERT_TRUE(profiles.is_array());
        ASSERT_EQ(profiles.size(), 1u);
        EXPECT_EQ(profiles[0]["profileId"], "0");
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

        nlohmann::json profile_data;
        profile_data["name"] = "test-profile";
        profile_data["active"] = true;
        auto update_result = sm.UpdateProfile("0", profile_data.dump());
        EXPECT_EQ(update_result, DAS_S_OK);

        auto result = sm.GetProfile("0");
        auto loaded = nlohmann::json::parse(result);
        EXPECT_EQ(loaded["name"], "test-profile");
        EXPECT_EQ(loaded["active"], true);
    }

    TEST_F(SettingsManagerTest, GetPluginSettings_ReturnsData_AfterUpdate)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json plugin_data;
        plugin_data["setting1"] = "value1";
        plugin_data["count"] = 42;
        auto update_result =
            sm.UpdatePluginSettings("0", "test-guid", plugin_data.dump());
        EXPECT_EQ(update_result, DAS_S_OK);

        auto result = sm.GetPluginSettings("0", "test-guid");
        auto loaded = nlohmann::json::parse(result);
        EXPECT_EQ(loaded["setting1"], "value1");
        EXPECT_EQ(loaded["count"], 42);
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
                        nlohmann::json data;
                        data["writer"] = w;
                        data["iteration"] = i;
                        sm.UpdateGlobalSettings(data.dump());
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
                        auto parsed = nlohmann::json::parse(result);
                        EXPECT_TRUE(parsed.is_object());
                    }
                });
        }

        for (auto& t : threads)
        {
            t.join();
        }

        // Verify final state is valid JSON
        auto final_result = sm.GetGlobalSettings();
        auto final_json = nlohmann::json::parse(final_result);
        EXPECT_TRUE(final_json.is_object());
        EXPECT_TRUE(final_json.contains("writer"));
        EXPECT_TRUE(final_json.contains("iteration"));
    }

    // --- Split-file plugin settings tests ---

    TEST_F(SettingsManagerTest, PluginSettings_UsesSplitFile)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json plugin_data;
        plugin_data["adbPath"] = "/usr/bin/adb";
        plugin_data["timeout"] = 30;
        auto result =
            sm.UpdatePluginSettingsJson("0", "plugin-guid-1", plugin_data);
        EXPECT_EQ(result, DAS_S_OK);

        // Verify a separate file was created
        auto guid_file = test_dir_ / "0" / "plugin-guid-1.json";
        EXPECT_TRUE(std::filesystem::exists(guid_file));

        // Read back through GetPluginSettingsJson
        auto loaded = sm.GetPluginSettingsJson("0", "plugin-guid-1");
        EXPECT_EQ(loaded["adbPath"], "/usr/bin/adb");
        EXPECT_EQ(loaded["timeout"], 30);
    }

    TEST_F(SettingsManagerTest, PluginSettings_NotInUiJson)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json plugin_data;
        plugin_data["key"] = "value";
        auto result =
            sm.UpdatePluginSettingsJson("0", "some-guid", plugin_data);
        EXPECT_EQ(result, DAS_S_OK);

        // ui.json should not contain plugin GUID data
        auto profile = sm.GetProfileJson("0");
        EXPECT_FALSE(profile.contains("some-guid"));
    }

    TEST_F(SettingsManagerTest, PluginSettings_FieldUpdate)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Set initial plugin settings
        nlohmann::json plugin_data;
        plugin_data["field1"] = "original";
        plugin_data["field2"] = 100;
        sm.UpdatePluginSettingsJson("0", "my-guid", plugin_data);

        // Update a single field
        auto result = sm.UpdatePluginSettingsFieldJson(
            "0",
            "my-guid",
            "field1",
            nlohmann::json("updated"));
        EXPECT_EQ(result, DAS_S_OK);

        // Verify field was updated
        auto field = sm.GetPluginSettingsFieldJson("0", "my-guid", "field1");
        EXPECT_EQ(field.get<std::string>(), "updated");

        // Verify other field unchanged
        auto field2 = sm.GetPluginSettingsFieldJson("0", "my-guid", "field2");
        EXPECT_EQ(field2, 100);
    }

    TEST_F(SettingsManagerTest, PluginSettings_PersistsAcrossInstances)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json plugin_data;
        plugin_data["setting"] = "persistent";
        sm.UpdatePluginSettingsJson("0", "guid-123", plugin_data);

        // New instance should read the same data
        Das::Core::SettingsManager::SettingsManager sm2(test_dir_);
        auto loaded = sm2.GetPluginSettingsJson("0", "guid-123");
        EXPECT_EQ(loaded["setting"], "persistent");
    }

    // --- Scheduler index tests (scheduler.json) ---

    TEST_F(SettingsManagerTest, SchedulerIndex_ReturnsDefault_WhenMissing)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        auto state = sm.GetSchedulerIndexJson("0");
        EXPECT_EQ(state["nextTaskId"], 0);
        EXPECT_TRUE(state["taskOrder"].is_array());
        EXPECT_EQ(state["taskOrder"].size(), 0u);
    }

    TEST_F(SettingsManagerTest, SchedulerIndex_PersistsNextTaskId)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json state;
        state["nextTaskId"] = 5;
        state["taskOrder"] = nlohmann::json::array();
        auto result = sm.UpdateSchedulerIndexJson("0", state);
        EXPECT_EQ(result, DAS_S_OK);

        // Verify separate file created
        auto scheduler_file = test_dir_ / "0" / "scheduler.json";
        EXPECT_TRUE(std::filesystem::exists(scheduler_file));

        // Read back
        auto loaded = sm.GetSchedulerIndexJson("0");
        EXPECT_EQ(loaded["nextTaskId"], 5);
        EXPECT_TRUE(loaded["taskOrder"].is_array());
    }

    TEST_F(SettingsManagerTest, SchedulerIndex_PersistsAcrossInstances)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json state;
        state["nextTaskId"] = 3;
        state["taskOrder"] = nlohmann::json::array({0, 1});
        sm.UpdateSchedulerIndexJson("0", state);

        // New instance reads the same state
        Das::Core::SettingsManager::SettingsManager sm2(test_dir_);
        auto loaded = sm2.GetSchedulerIndexJson("0");
        EXPECT_EQ(loaded["nextTaskId"], 3);
        ASSERT_EQ(loaded["taskOrder"].size(), 2u);
        EXPECT_EQ(loaded["taskOrder"][0], 0);
        EXPECT_EQ(loaded["taskOrder"][1], 1);
    }

    TEST_F(SettingsManagerTest, SchedulerIndex_OrderedTaskIdsPreserved)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json state;
        state["nextTaskId"] = 3;
        state["taskOrder"] = nlohmann::json::array({2, 0, 1});
        sm.UpdateSchedulerIndexJson("0", state);

        auto loaded = sm.GetSchedulerIndexJson("0");
        ASSERT_EQ(loaded["taskOrder"].size(), 3u);
        EXPECT_EQ(loaded["taskOrder"][0], 2);
        EXPECT_EQ(loaded["taskOrder"][1], 0);
        EXPECT_EQ(loaded["taskOrder"][2], 1);
    }

    TEST_F(SettingsManagerTest, SchedulerIndex_NotInUiJson)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json state;
        state["nextTaskId"] = 1;
        state["taskOrder"] = nlohmann::json::array();
        sm.UpdateSchedulerIndexJson("0", state);

        // ui.json should not contain scheduler data
        auto profile = sm.GetProfileJson("0");
        EXPECT_FALSE(profile.contains("scheduler"));
    }

    // --- Task instance tests (taskId{id}.json) ---

    TEST_F(SettingsManagerTest, TaskInstance_ReadWrite)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json task;
        task["id"] = 0;
        task["taskGuid"] = "B4F60C54-67DF-407A-B891-6D3C90CDB9A1";
        task["pluginGuid"] = "8F08935F-11B9-4D3A-BB0D-96D1862FE3F6";
        task["nextExecutionTime"] = nullptr;
        task["properties"] = {{"claimMail", true}, {"maxRetryCount", 3}};

        auto result = sm.UpdateTaskInstanceJson("0", 0, task);
        EXPECT_EQ(result, DAS_S_OK);

        // Verify file was created with expected name
        auto task_file = test_dir_ / "0" / "taskId0.json";
        EXPECT_TRUE(std::filesystem::exists(task_file));

        auto loaded = sm.GetTaskInstanceJson("0", 0);
        EXPECT_EQ(loaded["id"], 0);
        EXPECT_EQ(loaded["taskGuid"], "B4F60C54-67DF-407A-B891-6D3C90CDB9A1");
        EXPECT_EQ(loaded["properties"]["claimMail"], true);
        EXPECT_EQ(loaded["properties"]["maxRetryCount"], 3);
    }

    TEST_F(SettingsManagerTest, TaskInstance_DeleteRemovesFile)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json task;
        task["id"] = 5;
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

        nlohmann::json task;
        task["id"] = 0;
        task["taskGuid"] = "B4F60C54-67DF-407A-B891-6D3C90CDB9A1";
        task["properties"] = {{"claimMail", false}};
        sm.UpdateTaskInstanceJson("0", 0, task);

        Das::Core::SettingsManager::SettingsManager sm2(test_dir_);
        auto loaded = sm2.GetTaskInstanceJson("0", 0);
        EXPECT_EQ(loaded["id"], 0);
        EXPECT_EQ(loaded["properties"]["claimMail"], false);
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
        EXPECT_EQ(loaded["adbPath"], "/usr/bin/adb");
        EXPECT_EQ(loaded["timeout"], 30);
    }

    TEST_F(
        SettingsManagerTest,
        CorruptPluginSettings_WithExistingFile_RebuildFromDefaults)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Write valid JSON first
        nlohmann::json valid;
        valid["existingKey"] = "existingValue";
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
        EXPECT_EQ(loaded["existingKey"], "existingValue");
    }

    TEST_F(SettingsManagerTest, CorruptTaskInstance_DoesNotCorruptOtherFiles)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Create two task instance files
        nlohmann::json task0;
        task0["id"] = 0;
        task0["taskGuid"] = "guid-A";
        sm.UpdateTaskInstanceJson("0", 0, task0);

        nlohmann::json task1;
        task1["id"] = 1;
        task1["taskGuid"] = "guid-B";
        sm.UpdateTaskInstanceJson("0", 1, task1);

        // Corrupt task0's file
        auto task0_file = test_dir_ / "0" / "taskId0.json";
        {
            std::ofstream ofs{task0_file};
            ofs << "CORRUPT DATA {{{";
        }

        // task1 should still be readable
        auto loaded1 = sm.GetTaskInstanceJson("0", 1);
        EXPECT_EQ(loaded1["id"], 1);
        EXPECT_EQ(loaded1["taskGuid"], "guid-B");

        // ui.json should still be intact
        auto profile = sm.GetProfileJson("0");
        EXPECT_TRUE(profile.is_object());

        // scheduler.json should still be independent
        auto scheduler = sm.GetSchedulerIndexJson("0");
        EXPECT_EQ(scheduler["nextTaskId"], 0);
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
        EXPECT_EQ(loaded["key1"], "value1");
    }

    // --- Coexistence test ---

    TEST_F(SettingsManagerTest, AllSplitFilesCoexist_Independently)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Write to ui.json
        sm.UpdateProfileJson("0", {{"theme", "dark"}});

        // Write plugin settings
        sm.UpdatePluginSettingsJson(
            "0",
            "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E",
            {{"adbPath", "/usr/bin/adb"}});

        // Write scheduler index
        nlohmann::json scheduler;
        scheduler["nextTaskId"] = 1;
        scheduler["taskOrder"] = nlohmann::json::array({0});
        sm.UpdateSchedulerIndexJson("0", scheduler);

        // Write task instance
        nlohmann::json task;
        task["id"] = 0;
        task["taskGuid"] = "B4F60C54-67DF-407A-B891-6D3C90CDB9A1";
        sm.UpdateTaskInstanceJson("0", 0, task);

        // All files exist independently
        EXPECT_TRUE(std::filesystem::exists(test_dir_ / "0" / "ui.json"));
        EXPECT_TRUE(
            std::filesystem::exists(
                test_dir_ / "0" / "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E.json"));
        EXPECT_TRUE(
            std::filesystem::exists(test_dir_ / "0" / "scheduler.json"));
        EXPECT_TRUE(std::filesystem::exists(test_dir_ / "0" / "taskId0.json"));

        // Each reads back correctly
        EXPECT_EQ(sm.GetProfileJson("0")["theme"], "dark");
        EXPECT_EQ(
            sm.GetPluginSettingsJson(
                "0",
                "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E")["adbPath"],
            "/usr/bin/adb");
        EXPECT_EQ(sm.GetSchedulerIndexJson("0")["nextTaskId"], 1);
        EXPECT_EQ(
            sm.GetTaskInstanceJson("0", 0)["taskGuid"],
            "B4F60C54-67DF-407A-B891-6D3C90CDB9A1");
    }

} // anonymous namespace
