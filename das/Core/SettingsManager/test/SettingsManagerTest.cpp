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

    // --- Profile-root plugin settings tests ---

    TEST_F(SettingsManagerTest, PluginSettings_StoredInProfileRoot)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json plugin_data;
        plugin_data["adbPath"] = "/usr/bin/adb";
        plugin_data["timeout"] = 30;
        auto result =
            sm.UpdatePluginSettingsJson("0", "plugin-guid-1", plugin_data);
        EXPECT_EQ(result, DAS_S_OK);

        // Verify the GUID key is at the profile root
        auto profile = sm.GetProfileJson("0");
        ASSERT_TRUE(profile.contains("plugin-guid-1"));
        EXPECT_EQ(profile["plugin-guid-1"]["adbPath"], "/usr/bin/adb");
        EXPECT_EQ(profile["plugin-guid-1"]["timeout"], 30);

        // Read back through GetPluginSettingsJson
        auto loaded = sm.GetPluginSettingsJson("0", "plugin-guid-1");
        EXPECT_EQ(loaded["adbPath"], "/usr/bin/adb");
        EXPECT_EQ(loaded["timeout"], 30);
    }

    TEST_F(SettingsManagerTest, PluginSettings_NoSplitGuidFiles)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json plugin_data;
        plugin_data["key"] = "value";
        auto result =
            sm.UpdatePluginSettingsJson("0", "some-guid", plugin_data);
        EXPECT_EQ(result, DAS_S_OK);

        // Verify no split guid.json file was created
        auto guid_file = test_dir_ / "0" / "some-guid.json";
        EXPECT_FALSE(std::filesystem::exists(guid_file));

        // Only ui.json should exist in the profile directory
        auto ui_file = test_dir_ / "0" / "ui.json";
        EXPECT_TRUE(std::filesystem::exists(ui_file));
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

    // --- Scheduler state tests ---

    TEST_F(SettingsManagerTest, GetSchedulerState_ReturnsDefault_WhenMissing)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        auto state = sm.GetSchedulerStateJson("0");
        EXPECT_EQ(state["nextTaskId"], 0);
        EXPECT_TRUE(state["tasks"].is_array());
        EXPECT_EQ(state["tasks"].size(), 0u);
    }

    TEST_F(SettingsManagerTest, UpdateSchedulerState_PersistsNextTaskId)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json state;
        state["nextTaskId"] = 5;
        state["tasks"] = nlohmann::json::array();
        auto result = sm.UpdateSchedulerStateJson("0", state);
        EXPECT_EQ(result, DAS_S_OK);

        // Read back
        auto loaded = sm.GetSchedulerStateJson("0");
        EXPECT_EQ(loaded["nextTaskId"], 5);
        EXPECT_TRUE(loaded["tasks"].is_array());
    }

    TEST_F(SettingsManagerTest, SchedulerState_PersistsAcrossInstances)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json state;
        state["nextTaskId"] = 3;
        state["tasks"] = nlohmann::json::array();
        state["tasks"].push_back(
            {{"id", 0},
             {"taskGuid", "B4F60C54-67DF-407A-B891-6D3C90CDB9A1"},
             {"nextExecutionTime", nullptr},
             {"properties", {{"claimMail", true}}}});
        state["tasks"].push_back(
            {{"id", 1},
             {"taskGuid", "B4F60C54-67DF-407A-B891-6D3C90CDB9A1"},
             {"nextExecutionTime", nullptr},
             {"properties", {{"claimMail", false}}}});
        sm.UpdateSchedulerStateJson("0", state);

        // New instance reads the same state
        Das::Core::SettingsManager::SettingsManager sm2(test_dir_);
        auto loaded = sm2.GetSchedulerStateJson("0");
        EXPECT_EQ(loaded["nextTaskId"], 3);
        ASSERT_EQ(loaded["tasks"].size(), 2u);
        EXPECT_EQ(loaded["tasks"][0]["id"], 0);
        EXPECT_EQ(loaded["tasks"][1]["id"], 1);
        EXPECT_EQ(loaded["tasks"][0]["properties"]["claimMail"], true);
        EXPECT_EQ(loaded["tasks"][1]["properties"]["claimMail"], false);
    }

    TEST_F(SettingsManagerTest, SchedulerState_OrderedTasksPreserved)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json state;
        state["nextTaskId"] = 3;
        state["tasks"] = nlohmann::json::array();
        state["tasks"].push_back({{"id", 0}, {"name", "first"}});
        state["tasks"].push_back({{"id", 1}, {"name", "second"}});
        state["tasks"].push_back({{"id", 2}, {"name", "third"}});
        sm.UpdateSchedulerStateJson("0", state);

        auto loaded = sm.GetSchedulerStateJson("0");
        ASSERT_EQ(loaded["tasks"].size(), 3u);
        EXPECT_EQ(loaded["tasks"][0]["name"], "first");
        EXPECT_EQ(loaded["tasks"][1]["name"], "second");
        EXPECT_EQ(loaded["tasks"][2]["name"], "third");
    }

    TEST_F(SettingsManagerTest, SchedulerState_StoredInProfileRoot)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        nlohmann::json state;
        state["nextTaskId"] = 1;
        state["tasks"] = nlohmann::json::array();
        sm.UpdateSchedulerStateJson("0", state);

        // Verify scheduler is at profile root
        auto profile = sm.GetProfileJson("0");
        ASSERT_TRUE(profile.contains("scheduler"));
        EXPECT_EQ(profile["scheduler"]["nextTaskId"], 1);
    }

    TEST_F(SettingsManagerTest, PluginSettingsAndSchedulerCoexist)
    {
        Das::Core::SettingsManager::SettingsManager sm(test_dir_);
        sm.CreateProfile("0");

        // Write plugin settings
        nlohmann::json plugin_data;
        plugin_data["setting1"] = "value1";
        sm.UpdatePluginSettingsJson("0", "guid-A", plugin_data);

        // Write scheduler state
        nlohmann::json state;
        state["nextTaskId"] = 2;
        state["tasks"] = nlohmann::json::array();
        sm.UpdateSchedulerStateJson("0", state);

        // Both coexist in profile
        auto profile = sm.GetProfileJson("0");
        ASSERT_TRUE(profile.contains("guid-A"));
        ASSERT_TRUE(profile.contains("scheduler"));
        EXPECT_EQ(profile["guid-A"]["setting1"], "value1");
        EXPECT_EQ(profile["scheduler"]["nextTaskId"], 2);

        // Both readable through dedicated APIs
        auto loaded_settings = sm.GetPluginSettingsJson("0", "guid-A");
        EXPECT_EQ(loaded_settings["setting1"], "value1");
        auto loaded_state = sm.GetSchedulerStateJson("0");
        EXPECT_EQ(loaded_state["nextTaskId"], 2);
    }

} // anonymous namespace
