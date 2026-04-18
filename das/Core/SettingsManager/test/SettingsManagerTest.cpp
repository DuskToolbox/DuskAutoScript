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

} // anonymous namespace
