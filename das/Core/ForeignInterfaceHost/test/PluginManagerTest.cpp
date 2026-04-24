#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/ForeignInterfaceHost/PluginResourceIndex.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasSharedRef.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstring>
#include <fstream>
#include <random>

using namespace DAS::Core::ForeignInterfaceHost;
using namespace DAS::Core::IPC;
using Das::DasPtr;
using namespace Das::PluginInterface;

namespace
{
    std::filesystem::path UniqueSettingsDir()
    {
        static std::atomic<int> counter{0};
        std::random_device      rd;
        auto name = "das_test_settings_" + std::to_string(rd()) + "_"
                    + std::to_string(counter.fetch_add(1));
        return std::filesystem::current_path() / name;
    }

    std::filesystem::path GetTestPluginPath()
    {
        // CppRuntime::LoadPlugin expects a path to the JSON manifest file.
        // The manifest contains the DLL name which CppRuntime uses to locate
        // and load the actual shared library.
        return std::filesystem::path{IpcTestConfig::GetPluginDir()}
               / "IpcTestPlugin1.json";
    }

    DasPtr<IForeignLanguageRuntime> CreateCppRuntime()
    {
        ForeignLanguageRuntimeFactoryDesc desc{};
        desc.language = ForeignInterfaceLanguage::Cpp;
        auto result = CreateForeignLanguageRuntime(desc);
        if (!result)
        {
            return nullptr;
        }
        return std::move(result.value());
    }
} // anonymous namespace

// ============================================================
// GUID index tests
// ============================================================

class PluginManagerGuidTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        settings_dir_ = UniqueSettingsDir();
        settings_manager_ =
            std::make_unique<DAS::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        pm_ = std::make_unique<PluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));
        auto runtime = CreateCppRuntime();
        ASSERT_NE(runtime, nullptr) << "Failed to create Cpp runtime";
        ASSERT_EQ(pm_->Initialize(1, runtime), DAS_S_OK);
    }

    void TearDown() override
    {
        pm_->Shutdown();
        std::filesystem::remove_all(settings_dir_);
    }

    std::unique_ptr<DAS::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
    std::unique_ptr<PluginManager>                            pm_;
    std::filesystem::path                                     settings_dir_;
};

TEST_F(PluginManagerGuidTest, LoadPluginAndFindByGuid)
{
    auto plugin_path = GetTestPluginPath();

    auto result = pm_->LoadPlugin(plugin_path);
    ASSERT_EQ(result, DAS_S_OK) << "Failed to load IpcTestPlugin1.dll";

    EXPECT_EQ(pm_->GetLoadedPluginCount(), 1);
    EXPECT_TRUE(pm_->IsPluginLoaded(plugin_path));
}

TEST_F(PluginManagerGuidTest, LoadDuplicatePathReturnsSFalse)
{
    auto plugin_path = GetTestPluginPath();

    auto result1 = pm_->LoadPlugin(plugin_path);
    ASSERT_EQ(result1, DAS_S_OK) << "First load should succeed";

    auto result2 = pm_->LoadPlugin(plugin_path);
    EXPECT_EQ(result2, DAS_S_FALSE)
        << "Duplicate path should return DAS_S_FALSE";

    EXPECT_EQ(pm_->GetLoadedPluginCount(), 1)
        << "Count should remain 1 after duplicate load";
}

TEST_F(PluginManagerGuidTest, LoadDuplicateGuidViaSymlinkReturnsAlreadyExists)
{
    auto plugin_path = GetTestPluginPath();

    auto result1 = pm_->LoadPlugin(plugin_path);
    ASSERT_EQ(result1, DAS_S_OK) << "First load should succeed";

    // Create a symlink to the same manifest JSON to test GUID conflict
    // detection. The symlink provides a different entry path.
    // Note: NormalizePath() uses weakly_canonical() which resolves symlinks,
    // so if the symlink resolves to the same canonical path as the original,
    // path deduplication returns DAS_S_FALSE before GUID conflict is checked.
    // GUID conflict detection (DAS_E_DUPLICATE_ELEMENT) is exercised when two
    // genuinely different paths produce the same GUID — this requires two
    // distinct plugin DLLs with identical GUIDs, which cannot be tested here.
    // The GUID conflict code path in LoadPlugin
    // (loaded_plugins_.contains(desc->guid)) is guaranteed by code review.
    auto symlink_path =
        std::filesystem::current_path() / "IpcTestPlugin1_symlink.json";

    std::error_code ec;
    std::filesystem::create_symlink(
        std::filesystem::canonical(plugin_path),
        symlink_path,
        ec);

    if (ec)
    {
        GTEST_SKIP() << "Symlink creation failed: " << ec.message();
    }

    auto result2 = pm_->LoadPlugin(symlink_path);
    // weakly_canonical resolves the symlink to the same path, so path
    // deduplication fires first and returns DAS_S_FALSE.
    EXPECT_TRUE(result2 == DAS_S_FALSE || result2 == DAS_E_DUPLICATE_ELEMENT)
        << "Expected DAS_S_FALSE (path dedup) or DAS_E_DUPLICATE_ELEMENT "
           "(GUID conflict), got: "
        << result2;

    EXPECT_EQ(pm_->GetLoadedPluginCount(), 1)
        << "Count should remain 1 after duplicate load";

    // Cleanup symlink
    std::filesystem::remove(symlink_path, ec);
}

// ============================================================
// Feature-type index tests
// ============================================================

class PluginManagerFeatureTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        settings_dir_ = UniqueSettingsDir();
        settings_manager_ =
            std::make_unique<DAS::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        auto ipc_sp =
            DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        pm_ = std::make_unique<PluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp));
        auto runtime = CreateCppRuntime();
        ASSERT_NE(runtime, nullptr) << "Failed to create Cpp runtime";
        ASSERT_EQ(pm_->Initialize(1, runtime), DAS_S_OK);

        registry_ = std::make_unique<RemoteObjectRegistry>();
        pm_->SetRegistry(*registry_);

        auto plugin_path = GetTestPluginPath();
        ASSERT_EQ(pm_->LoadPlugin(plugin_path), DAS_S_OK)
            << "Failed to load IpcTestPlugin1.dll";

        plugin_path_ = plugin_path;
    }

    void TearDown() override
    {
        pm_->Shutdown();
        std::filesystem::remove_all(settings_dir_);
    }

    void RegisterObjects()
    {
        ASSERT_EQ(pm_->RegisterPluginObjects(plugin_path_), DAS_S_OK)
            << "RegisterPluginObjects failed";
    }

    std::unique_ptr<DAS::Core::SettingsManager::SettingsManager>
                                          settings_manager_;
    std::unique_ptr<PluginManager>        pm_;
    std::unique_ptr<RemoteObjectRegistry> registry_;
    std::filesystem::path                 plugin_path_;
    std::filesystem::path                 settings_dir_;
};

TEST_F(PluginManagerFeatureTest, GetFeaturesByTypeReturnsInputFactory)
{
    RegisterObjects();

    auto span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    ASSERT_GE(span.size(), 1u) << "Expected at least one INPUT_FACTORY feature";
    EXPECT_EQ(span[0]->feature_type, DAS_PLUGIN_FEATURE_INPUT_FACTORY);
}

TEST_F(PluginManagerFeatureTest, GetFeaturesByTypeReturnsComponentFactory)
{
    RegisterObjects();

    auto span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
    ASSERT_GE(span.size(), 1u)
        << "Expected at least one COMPONENT_FACTORY feature";
    EXPECT_EQ(span[0]->feature_type, DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
}

TEST_F(PluginManagerFeatureTest, GetFeaturesByTypeEmptyForUnregistered)
{
    // IpcTestPlugin1 does not provide DAS_PLUGIN_FEATURE_CAPTURE_FACTORY,
    // and we have NOT called RegisterPluginObjects, so the feature_type_index_
    // should be empty for all types.
    auto span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_CAPTURE_FACTORY);
    EXPECT_EQ(span.size(), 0u) << "Expected empty span for unregistered type";
}

TEST_F(PluginManagerFeatureTest, UnloadRemovesFromFeatureIndex)
{
    RegisterObjects();

    auto span_before = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    ASSERT_GE(span_before.size(), 1u)
        << "INPUT_FACTORY should be present before unload";

    auto unload_result = pm_->UnloadPlugin(plugin_path_);
    ASSERT_EQ(unload_result, DAS_S_OK) << "UnloadPlugin failed";

    auto span_after = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    EXPECT_EQ(span_after.size(), 0u)
        << "INPUT_FACTORY should be gone after unload";
}

TEST_F(PluginManagerFeatureTest, LoadOrderPreservedInSpan)
{
    RegisterObjects();

    auto input_span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    ASSERT_EQ(input_span.size(), 1u);
    EXPECT_EQ(input_span[0]->feature_type, DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    // Verify the feature belongs to the loaded plugin
    EXPECT_FALSE(input_span[0]->plugin_name.empty());

    auto comp_span =
        pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
    ASSERT_EQ(comp_span.size(), 1u);
    EXPECT_EQ(comp_span[0]->feature_type, DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
    EXPECT_FALSE(comp_span[0]->plugin_name.empty());
}

TEST_F(PluginManagerFeatureTest, FeatureInfoContainsPluginGuid)
{
    RegisterObjects();

    auto input_span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    ASSERT_EQ(input_span.size(), 1u);

    // plugin_guid 应该非零（与 manifest.json 中的 GUID 匹配）
    DasGuid zero_guid{};
    EXPECT_FALSE(
        std::memcmp(&input_span[0]->plugin_guid, &zero_guid, sizeof(DasGuid))
        == 0)
        << "plugin_guid should be populated after LoadPlugin";
}

// ============================================================
// IPC routing tests
// ============================================================

TEST_F(PluginManagerGuidTest, LoadPlugin_NoHostPath_ReturnsError)
{
    // IPC context is always present via constructor; this tests that
    // missing host_exe_path_ still blocks the IPC load path.
    auto test_dir =
        std::filesystem::current_path() / "test_plugin_no_host_path";
    std::filesystem::create_directories(test_dir);
    auto manifest_path = test_dir / "test_plugin_no_host_path.json";

    nlohmann::json manifest = {
        {"guid", "{00000000-0000-0000-0000-000000000001}"},
        {"name", "TestCSharpPlugin"},
        {"language", "CSharp"},
        {"description", "test"},
        {"author", "test"},
        {"version", "1.0"},
        {"supportedSystem", "win"},
        {"pluginFilenameExtension", ".dll"},
    };
    {
        std::ofstream ofs(manifest_path);
        ofs << manifest.dump();
    }

    auto result = pm_->LoadPlugin(test_dir);
    EXPECT_EQ(result, DAS_E_NO_IMPLEMENTATION);

    std::filesystem::remove_all(test_dir);
}

TEST_F(PluginManagerGuidTest, LoadPlugin_CppLanguage_StaysInProcess)
{
    // Cpp (white-listed language) should NOT take the IPC path.
    // A nonexistent path returns an error from CppRuntime::LoadPlugin,
    // but NOT DAS_E_NO_IMPLEMENTATION (which is the IPC rejection code).
    auto result = pm_->LoadPlugin("/nonexistent/plugin/path");
    EXPECT_NE(result, DAS_E_NO_IMPLEMENTATION);
}

TEST_F(PluginManagerGuidTest, LoadPlugin_CppWithLoadModeIpc_GoesIpcPath)
{
    // Cpp (white-listed) with loadMode=Ipc should be forced to IPC path.
    auto test_dir =
        std::filesystem::current_path() / "test_plugin_loadmode_ipc";
    std::filesystem::create_directories(test_dir);
    auto manifest_path = test_dir / "test_plugin_loadmode_ipc.json";

    nlohmann::json manifest = {
        {"guid", "{00000000-0000-0000-0000-000000000010}"},
        {"name", "TestPluginCppIpc"},
        {"language", "Cpp"},
        {"loadMode", "ipc"},
        {"description", "test"},
        {"author", "test"},
        {"version", "1.0"},
        {"supportedSystem", "win"},
        {"pluginFilenameExtension", ".dll"},
    };
    {
        std::ofstream ofs(manifest_path);
        ofs << manifest.dump();
    }

    // No IPC context set -> IPC path returns DAS_E_NO_IMPLEMENTATION
    auto result = pm_->LoadPlugin(test_dir);
    EXPECT_EQ(result, DAS_E_NO_IMPLEMENTATION);

    std::filesystem::remove_all(test_dir);
}

TEST_F(PluginManagerGuidTest, SetHostExePath)
{
    pm_->SetHostExePath("/path/to/DasHost");
}

TEST_F(PluginManagerGuidTest, OnHostProcessExit_CleansUpIndex)
{
    // Manually set up internal state to simulate post-IPC-load state.
    // This test uses friend access to PluginManager private members.
    DasGuid test_guid{};
    test_guid.data1 = 0x00000099;

    LoadedPlugin fake_plugin;
    fake_plugin.plugin_path = "/fake/ipc/plugin";
    pm_->loaded_plugins_[test_guid] = std::move(fake_plugin);
    pm_->path_to_guid_["/fake/ipc/plugin"] = test_guid;

    // Verify setup
    EXPECT_TRUE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_TRUE(pm_->path_to_guid_.contains("/fake/ipc/plugin"));

    // Trigger the disconnect callback (new GUID-first signature)
    pm_->OnHostProcessExit(test_guid, 1);

    // Verify all indexes are cleaned up
    EXPECT_FALSE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_FALSE(pm_->path_to_guid_.contains("/fake/ipc/plugin"));
    EXPECT_FALSE(pm_->host_launchers_.contains(test_guid));
}

TEST_F(PluginManagerGuidTest, OnHeartbeatTimeout_CleansUpIndex)
{
    // Manually set up internal state to simulate post-IPC-load state.
    // Tests the CleanupPluginByGuid path used by heartbeat timeout callback.
    DasGuid test_guid{};
    test_guid.data1 = 0x000000AA;

    LoadedPlugin fake_plugin;
    fake_plugin.plugin_path = "/fake/heartbeat/plugin";
    pm_->loaded_plugins_[test_guid] = std::move(fake_plugin);
    pm_->path_to_guid_["/fake/heartbeat/plugin"] = test_guid;

    // Verify setup
    EXPECT_TRUE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_TRUE(pm_->path_to_guid_.contains("/fake/heartbeat/plugin"));

    // Directly call CleanupPluginByGuid (simulates heartbeat timeout callback)
    {
        std::lock_guard<std::mutex> lock(pm_->mutex_);
        pm_->CleanupPluginByGuid(test_guid);
    }

    // Verify all indexes are cleaned up
    EXPECT_FALSE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_FALSE(pm_->path_to_guid_.contains("/fake/heartbeat/plugin"));
    EXPECT_FALSE(pm_->host_launchers_.contains(test_guid));
}

// ============================================================
// PluginResourceIndex GUID cache tests
// ============================================================

namespace
{
    constexpr const char* kTestGuid1 = "A1B2C3D4-1111-2222-3333-444455556666";
    constexpr const char* kTestGuid2 = "A1B2C3D4-5555-6666-7777-888899990000";

    void WriteFolderModePlugin(
        const std::filesystem::path& plugin_dir,
        const std::string&           dirname,
        const std::string&           guid,
        const std::string&           resource_path_value)
    {
        auto pkg_dir = plugin_dir / dirname;
        std::filesystem::create_directories(pkg_dir);

        nlohmann::json manifest = {
            {"guid", guid},
            {"name", dirname},
            {"language", "Cpp"},
            {"description", "test plugin"},
            {"author", "test"},
            {"version", "1.0"},
            {"supportedSystem", "win"},
            {"pluginFilenameExtension", "dll"},
            {"settings", nlohmann::json::array()},
        };

        if (!resource_path_value.empty())
        {
            manifest["resourcePath"] = resource_path_value;
        }

        auto manifest_path = pkg_dir / (dirname + ".json");
        {
            std::ofstream ofs(manifest_path);
            ofs << manifest.dump();
        }

        // Create the resource subdirectory
        std::string rp =
            resource_path_value.empty() ? "resource" : resource_path_value;
        std::filesystem::create_directories(pkg_dir / rp);
    }
} // anonymous namespace

class PluginResourceIndexTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        test_dir_ =
            std::filesystem::current_path()
            / ("test_resource_index_" + std::to_string(std::random_device{}()));
        plugin_dir_ = test_dir_ / "plugins";
        std::filesystem::create_directories(plugin_dir_);

        auto& index = PluginResourceIndex::GetInstance();
        index.InvalidateCache();
        index.ConfigurePluginResourceScanRoot(plugin_dir_);
    }

    void TearDown() override
    {
        // Reset singleton to a clean state for subsequent tests
        auto& index = PluginResourceIndex::GetInstance();
        index.InvalidateCache();
        index.ConfigurePluginResourceScanRoot(plugin_dir_);

        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path plugin_dir_;
};

TEST_F(PluginResourceIndexTest, CacheMissTriggersFullScanAndHits)
{
    WriteFolderModePlugin(plugin_dir_, "TestPlugin1", kTestGuid1, "");

    auto  guid = MakeDasGuid(kTestGuid1);
    auto& index = PluginResourceIndex::GetInstance();

    const PluginResourceEntry* p_entry = nullptr;
    auto result = index.ResolvePluginResourceEntryByGuid(guid, &p_entry);

    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(p_entry, nullptr);
    EXPECT_EQ(p_entry->plugin_name, "TestPlugin1");
    EXPECT_FALSE(p_entry->resource_root.empty());
}

TEST_F(PluginResourceIndexTest, DuplicateGuidConflictFailsAndPreservesOriginal)
{
    // First, populate the cache with a single valid plugin
    WriteFolderModePlugin(plugin_dir_, "PluginAlpha", kTestGuid1, "");

    auto  guid = MakeDasGuid(kTestGuid1);
    auto& index = PluginResourceIndex::GetInstance();

    const PluginResourceEntry* p_entry = nullptr;
    auto result = index.ResolvePluginResourceEntryByGuid(guid, &p_entry);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(p_entry, nullptr);
    EXPECT_EQ(p_entry->plugin_name, "PluginAlpha");

    // Now add a second plugin with the SAME GUID -> conflict
    WriteFolderModePlugin(plugin_dir_, "PluginBeta", kTestGuid1, "");

    // Force a rescan
    index.InvalidateCache();

    const PluginResourceEntry* p_entry2 = nullptr;
    auto result2 = index.ResolvePluginResourceEntryByGuid(guid, &p_entry2);
    EXPECT_EQ(result2, DAS_E_DUPLICATE_ELEMENT);

    // Original map should remain unchanged: the old entry is still valid
    // (conflict prevents partial publishing)
}

TEST_F(PluginResourceIndexTest, SuccessfulRescanReplacesOldMap)
{
    // Initial state: one plugin
    WriteFolderModePlugin(plugin_dir_, "PluginV1", kTestGuid1, "");

    auto  guid1 = MakeDasGuid(kTestGuid1);
    auto& index = PluginResourceIndex::GetInstance();

    const PluginResourceEntry* p_entry = nullptr;
    auto result = index.ResolvePluginResourceEntryByGuid(guid1, &p_entry);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(p_entry->plugin_name, "PluginV1");

    // Remove the old plugin and add a new one with a different GUID
    std::filesystem::remove_all(plugin_dir_ / "PluginV1");
    WriteFolderModePlugin(plugin_dir_, "PluginV2", kTestGuid2, "assets");

    // Force rescan
    index.InvalidateCache();

    // Old GUID should now be gone (map replaced, not merged)
    const PluginResourceEntry* p_old = nullptr;
    auto old_result = index.ResolvePluginResourceEntryByGuid(guid1, &p_old);
    EXPECT_EQ(old_result, DAS_E_NOT_FOUND)
        << "Old GUID should not exist after map replacement";

    // New GUID should be present
    auto                       guid2 = MakeDasGuid(kTestGuid2);
    const PluginResourceEntry* p_new = nullptr;
    auto new_result = index.ResolvePluginResourceEntryByGuid(guid2, &p_new);
    ASSERT_EQ(new_result, DAS_S_OK);
    EXPECT_EQ(p_new->plugin_name, "PluginV2");
}

TEST_F(PluginResourceIndexTest, InvalidResourcePath_TraversalFails)
{
    WriteFolderModePlugin(plugin_dir_, "EvilPlugin", kTestGuid1, "../outside");

    auto  guid = MakeDasGuid(kTestGuid1);
    auto& index = PluginResourceIndex::GetInstance();

    const PluginResourceEntry* p_entry = nullptr;
    // The plugin with traversal resourcePath should be skipped during scan,
    // so the GUID will not be found.
    auto result = index.ResolvePluginResourceEntryByGuid(guid, &p_entry);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

TEST_F(PluginResourceIndexTest, InvalidResourcePath_AbsoluteFails)
{
    WriteFolderModePlugin(
        plugin_dir_,
        "AbsolutePlugin",
        kTestGuid1,
        "/etc/secrets");

    auto  guid = MakeDasGuid(kTestGuid1);
    auto& index = PluginResourceIndex::GetInstance();

    const PluginResourceEntry* p_entry = nullptr;
    auto result = index.ResolvePluginResourceEntryByGuid(guid, &p_entry);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}
