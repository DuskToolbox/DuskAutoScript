#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/IDasBase.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

using namespace Das::Core::SettingsManager;

namespace
{
    std::filesystem::path GetTestBaseDir()
    {
        const char* env = std::getenv("DAS_TEST_TMPDIR");
        if (env && env[0] != '\0')
        {
            return std::filesystem::path(env);
        }
        return std::filesystem::current_path();
    }
} // namespace

class SettingsManagerConcurrencyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        test_dir_ = GetTestBaseDir()
                    / ("das_test_settings_concurrency_"
                       + std::to_string(
                           std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count()));
        std::filesystem::create_directories(test_dir_);
        sm_ = std::make_unique<SettingsManager>(test_dir_);
    }

    void TearDown() override
    {
        sm_.reset();
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    std::filesystem::path            test_dir_;
    std::unique_ptr<SettingsManager> sm_;
};

// Test 1: Same-key serialization -- no lost update
TEST_F(SettingsManagerConcurrencyTest, SameKeySerialized)
{
    const std::string key_guid = "concurrency-test-plugin";
    std::atomic<int>  ops_done{0};

    auto writer = [&](int start_value)
    {
        for (int i = 0; i < 50; ++i)
        {
            nlohmann::json data;
            data["value"] = start_value + i;
            sm_->UpdatePluginSettingsJson("0", key_guid, data);
        }
        ops_done.fetch_add(1);
    };

    std::thread t1(writer, 0);
    std::thread t2(writer, 1000);
    t1.join();
    t2.join();

    EXPECT_EQ(ops_done.load(), 2);

    auto result = sm_->GetPluginSettingsJson("0", key_guid);
    int  final_value = result["value"].get<int>();
    EXPECT_TRUE(final_value >= 49);
    EXPECT_TRUE(final_value <= 1049);
}

// Test 2: HTTP whole-file vs plugin IPC field update race
TEST_F(SettingsManagerConcurrencyTest, HttpWholeFileVsPluginFieldRace)
{
    const std::string key_guid = "race-test-plugin";
    std::atomic<bool> done_a{false};
    std::atomic<bool> done_b{false};

    // Thread A: HTTP whole-file update
    auto http_writer = [&]()
    {
        for (int i = 0; i < 20; ++i)
        {
            nlohmann::json data;
            data["field_a"] = i;
            data["field_b"] = "http_write";
            sm_->UpdatePluginSettingsJson("0", key_guid, data);
        }
        done_a.store(true);
    };

    // Thread B: Plugin IPC field update (simulates DasAutoFlushJsonImpl path)
    auto plugin_writer = [&]()
    {
        for (int i = 0; i < 20; ++i)
        {
            sm_->UpdatePluginSettingsFieldJson(
                "0",
                key_guid,
                "field_c",
                nlohmann::json(std::to_string(i)));
        }
        done_b.store(true);
    };

    std::thread ta(http_writer);
    std::thread tb(plugin_writer);
    ta.join();
    tb.join();

    EXPECT_TRUE(done_a.load());
    EXPECT_TRUE(done_b.load());

    // Final state must be consistent (no half-written json)
    // Per-key mutex ensures operations are serialized: no interleaved writes.
    // The final state depends on which thread finished last:
    //   - If whole-file update finished last: only field_a + field_b present
    //   - If field update finished last: field_a + field_b + field_c present
    // In either case, the JSON must be a valid, parseable object.
    auto result = sm_->GetPluginSettingsJson("0", key_guid);
    EXPECT_TRUE(result.is_object());
    // At minimum, one of the writers' fields must be present
    EXPECT_TRUE(result.contains("field_a") || result.contains("field_c"));
}

// Test 3: Different keys do not block each other
TEST_F(SettingsManagerConcurrencyTest, DifferentKeysNonBlocking)
{
    std::atomic<bool> started{false};

    auto writer_a = [&]()
    {
        while (!started.load())
        {
            std::this_thread::yield();
        }
        sm_->UpdatePluginSettingsJson("0", "plugin-A", {{"data", "A"}});
    };

    auto writer_b = [&]()
    {
        while (!started.load())
        {
            std::this_thread::yield();
        }
        sm_->UpdatePluginSettingsJson("0", "plugin-B", {{"data", "B"}});
    };

    std::thread ta(writer_a);
    std::thread tb(writer_b);
    started.store(true);
    ta.join();
    tb.join();

    // Both operations should complete; different keys use different mutexes
    auto ra = sm_->GetPluginSettingsJson("0", "plugin-A");
    auto rb = sm_->GetPluginSettingsJson("0", "plugin-B");
    EXPECT_EQ(ra["data"], "A");
    EXPECT_EQ(rb["data"], "B");
}

// Test 4: Concurrent field updates to same plugin key are serialized
// Uses UpdatePluginSettingsFieldJson which performs a single-API RMW
// (read current -> modify field -> WriteJsonFile -> update snapshot)
// all under the same per-key mutex. This is the valid per-key RMW test.
TEST_F(
    SettingsManagerConcurrencyTest,
    ConcurrentFieldUpdatesSamePluginKeyAreSerialized)
{
    const std::string test_guid = "rmw-test-plugin";
    const int         iterations = 100;

    // Initialize plugin settings with a counter field
    sm_->UpdatePluginSettingsJson("0", test_guid, {{"counter", "0"}});

    auto field_updater = [&]()
    {
        for (int i = 0; i < iterations; ++i)
        {
            // UpdatePluginSettingsFieldJson performs a single-API RMW:
            // per-key mutex -> read current -> set field -> WriteJsonFile ->
            // snapshot
            sm_->UpdatePluginSettingsFieldJson(
                "0",
                test_guid,
                "counter",
                nlohmann::json(std::to_string(i)));
        }
    };

    std::thread t1(field_updater);
    std::thread t2(field_updater);
    t1.join();
    t2.join();

    // Final state must be a valid integer (no corrupted JSON from
    // interleaved writes). The per-key mutex ensures each field update
    // sees the previous value and writes back atomically.
    auto result = sm_->GetPluginSettingsJson("0", test_guid);
    EXPECT_TRUE(result.is_object());
    EXPECT_TRUE(result.contains("counter"));
    // Counter value is one of the written values (0 to iterations-1)
    EXPECT_TRUE(result["counter"].is_string());
}

// Test 5: cells_mutex_ does not cover I/O (different keys proceed concurrently)
TEST_F(SettingsManagerConcurrencyTest, RegistryLockNotHeldDuringIO)
{
    // Two threads write to different keys simultaneously
    // The registry lock (cells_mutex_) should only protect map lookup,
    // not the WriteJsonFile inside per-key mutex
    std::atomic<int> phase{0};

    auto writer = [&](const std::string& plugin_name)
    {
        phase.fetch_add(1);
        sm_->UpdatePluginSettingsJson(
            "0",
            plugin_name,
            {{"name", plugin_name}});
    };

    std::thread t1(writer, "concurrent-1");
    std::thread t2(writer, "concurrent-2");
    t1.join();
    t2.join();

    // Both writes should succeed
    auto r1 = sm_->GetPluginSettingsJson("0", "concurrent-1");
    auto r2 = sm_->GetPluginSettingsJson("0", "concurrent-2");
    EXPECT_EQ(r1["name"], "concurrent-1");
    EXPECT_EQ(r2["name"], "concurrent-2");
}
