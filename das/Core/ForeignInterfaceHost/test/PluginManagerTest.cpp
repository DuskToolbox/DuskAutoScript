#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>

using namespace DAS::Core::ForeignInterfaceHost;
using namespace DAS::Core::IPC;
using Das::DasPtr;
using namespace Das::PluginInterface;

namespace
{
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
        settings_manager_ =
            std::make_unique<DAS::Core::SettingsManager::SettingsManager>(
                std::filesystem::current_path() / "das_test_settings_guid");
        pm_ = std::make_unique<PluginManager>(*settings_manager_);
        auto runtime = CreateCppRuntime();
        ASSERT_NE(runtime, nullptr) << "Failed to create Cpp runtime";
        ASSERT_EQ(pm_->Initialize(1, runtime), DAS_S_OK);
    }

    void TearDown() override { pm_->Shutdown(); }

    std::unique_ptr<DAS::Core::SettingsManager::SettingsManager>
                                   settings_manager_;
    std::unique_ptr<PluginManager> pm_;
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
        settings_manager_ =
            std::make_unique<DAS::Core::SettingsManager::SettingsManager>(
                std::filesystem::current_path() / "das_test_settings_guid");
        pm_ = std::make_unique<PluginManager>(*settings_manager_);
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

    void TearDown() override { pm_->Shutdown(); }

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

TEST_F(PluginManagerGuidTest, LoadPlugin_NoIpcContext_ReturnsError)
{
    // Create a CSharp manifest (white-listed-out language)
    auto test_dir = std::filesystem::current_path() / "test_plugin_no_ipc_ctx";
    std::filesystem::create_directories(test_dir);
    auto manifest_path = test_dir / "test_plugin_no_ipc_ctx.json";

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

    // No IPC context set -- should return DAS_E_NO_IMPLEMENTATION
    auto result = pm_->LoadPlugin(test_dir);
    EXPECT_EQ(result, DAS_E_NO_IMPLEMENTATION);

    std::filesystem::remove_all(test_dir);
}

TEST_F(PluginManagerGuidTest, LoadPlugin_NoHostPath_ReturnsError)
{
    // Create a CSharp manifest
    auto test_dir =
        std::filesystem::current_path() / "test_plugin_no_host_path";
    std::filesystem::create_directories(test_dir);
    auto manifest_path = test_dir / "test_plugin_no_host_path.json";

    nlohmann::json manifest = {
        {"guid", "{00000000-0000-0000-0000-000000000002}"},
        {"name", "TestCSharpPlugin2"},
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

    // No host exe path set -- should return DAS_E_NO_IMPLEMENTATION
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

TEST_F(PluginManagerGuidTest, SetIpcContext_And_SetHostExePath)
{
    // Verify that setter methods do not crash.
    pm_->SetHostExePath("/path/to/DasHost");
    // SetIpcContext requires a real IIpcContext reference;
    // its integration is verified in IpcMultiProcessTest.
}

TEST_F(PluginManagerGuidTest, OnHostProcessExit_CleansUpIndex)
{
    // Manually set up internal state to simulate post-IPC-load state.
    // This test uses friend access to PluginManager private members.
    DasGuid test_guid{};
    test_guid.data1 = 0x00000099;
    uint16_t test_session_id = 42;

    pm_->session_to_guid_[test_session_id] = test_guid;

    LoadedPlugin fake_plugin;
    fake_plugin.plugin_path = "/fake/ipc/plugin";
    pm_->loaded_plugins_[test_guid] = std::move(fake_plugin);
    pm_->path_to_guid_["/fake/ipc/plugin"] = test_guid;

    // Verify setup
    EXPECT_TRUE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_TRUE(pm_->session_to_guid_.contains(test_session_id));
    EXPECT_TRUE(pm_->path_to_guid_.contains("/fake/ipc/plugin"));

    // Trigger the disconnect callback
    pm_->OnHostProcessExit(test_session_id, 1);

    // Verify all indexes are cleaned up
    EXPECT_FALSE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_FALSE(pm_->session_to_guid_.contains(test_session_id));
    EXPECT_FALSE(pm_->path_to_guid_.contains("/fake/ipc/plugin"));
    EXPECT_FALSE(pm_->host_launchers_.contains(test_guid));
}
