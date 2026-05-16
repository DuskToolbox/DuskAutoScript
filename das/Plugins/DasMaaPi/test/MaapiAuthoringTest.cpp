#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
    using namespace Das;
    using namespace Das::Core::ForeignInterfaceHost;
    using namespace Das::Plugins::DasMaaPi;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY;

    std::filesystem::path PluginPackageDir()
    {
        return std::filesystem::path{IpcTestConfig::GetPluginDir()}
               / "DasMaaPi";
    }

    std::filesystem::path MaapiManifestPath()
    {
        return PluginPackageDir() / "DasMaaPi.json";
    }

    std::filesystem::path FixturePath(std::string_view name)
    {
        return std::filesystem::current_path() / "DasMaaPi" / "test"
               / "fixtures" / std::filesystem::path{name};
    }

    DasPtr<IForeignLanguageRuntime> CreateCppRuntime()
    {
        ForeignLanguageRuntimeFactoryDesc desc{};
        desc.language = ForeignInterfaceLanguage::Cpp;
        auto runtime_result = CreateForeignLanguageRuntime(desc);
        return runtime_result ? std::move(runtime_result.value()) : nullptr;
    }

    DasPtr<Das::ExportInterface::IDasJson> ParseDasJson(std::string json)
    {
        DasPtr<Das::ExportInterface::IDasJson> result;
        EXPECT_EQ(ParseDasJsonFromString(json.c_str(), result.Put()), DAS_S_OK);
        return result;
    }

    yyjson::value ReadJsonInterface(Das::ExportInterface::IDasJson* json)
    {
        DasPtr<IDasReadOnlyString> text;
        if (!json || DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return Das::Utils::MakeYyjsonObject();
        }
        const char* raw = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&raw)) || raw == nullptr)
        {
            return Das::Utils::MakeYyjsonObject();
        }
        auto parsed = Das::Utils::ParseYyjsonFromString(raw);
        return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
    }

    bool ArrayHasFieldWithValuePath(
        const yyjson::value& document,
        std::string_view     value_path)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("schema")))
        {
            return false;
        }
        auto schema = (*obj)[std::string_view("schema")].as_object();
        if (!schema || !schema->contains(std::string_view("fields")))
        {
            return false;
        }
        auto fields = (*schema)[std::string_view("fields")].as_array();
        if (!fields)
        {
            return false;
        }
        for (auto it = fields->begin(); it != fields->end(); ++it)
        {
            auto field = it->as_object();
            if (field && field->contains(std::string_view("valuePath"))
                && (*field)[std::string_view("valuePath")]
                       .as_string()
                       .value_or("")
                       == value_path)
            {
                return true;
            }
        }
        return false;
    }

    std::filesystem::path UniqueSettingsDir()
    {
        const auto ticks =
            std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path()
               / ("das_maapi_authoring_" + std::to_string(ticks));
    }

    class MaapiAuthoringFixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            settings_dir_ = UniqueSettingsDir();
            settings_manager_ =
                std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                    settings_dir_);
            auto ipc_sp =
                Das::Core::IPC::MainProcess::CreateIpcContextShared(false);
            plugin_manager_ = std::make_unique<PluginManager>(
                *settings_manager_,
                Das::DasSharedRef<Das::Core::IPC::MainProcess::IIpcContext>(
                    ipc_sp));
            auto runtime = CreateCppRuntime();
            ASSERT_NE(runtime, nullptr);
            ASSERT_EQ(plugin_manager_->Initialize(1, runtime), DAS_S_OK);
            registry_ =
                std::make_unique<Das::Core::IPC::RemoteObjectRegistry>();
            plugin_manager_->SetRegistry(*registry_);
            ASSERT_EQ(plugin_manager_->LoadPlugin(MaapiManifestPath()), DAS_S_OK);
            ASSERT_EQ(
                plugin_manager_->RegisterPluginObjects(MaapiManifestPath()),
                DAS_S_OK);
        }

        void TearDown() override
        {
            if (plugin_manager_)
            {
                plugin_manager_->Shutdown();
            }
            std::filesystem::remove_all(settings_dir_);
        }

        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> CreateSession(
            std::string context = "{}")
        {
            auto features = plugin_manager_->GetFeaturesByType(
                DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);
            EXPECT_EQ(features.size(), 1u);
            DasPtr<Das::PluginInterface::IDasTaskAuthoringSessionFactory>
                factory;
            EXPECT_EQ(
                features[0]->interface_ptr->QueryInterface(
                    DasIidOf<
                        Das::PluginInterface::
                            IDasTaskAuthoringSessionFactory>(),
                    reinterpret_cast<void**>(factory.Put())),
                DAS_S_OK);
            auto context_json = ParseDasJson(std::move(context));
            DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
            EXPECT_EQ(
                factory->CreateSession(
                    MakeDasGuid(std::string(kTaskGuidText)),
                    context_json.Get(),
                    session.Put()),
                DAS_S_OK);
            return session;
        }

        std::filesystem::path settings_dir_;
        std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
        std::unique_ptr<Das::Core::IPC::RemoteObjectRegistry> registry_;
        std::unique_ptr<PluginManager>                        plugin_manager_;
    };
} // namespace

TEST_F(MaapiAuthoringFixture, ApplyPathFailurePersistsDiagnostics)
{
    auto session = CreateSession();
    auto missing_path = FixturePath("missing.jsonc").generic_string();
    auto change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"adapter.interfacePath\",\"value\":\""
        + missing_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto accepted =
        (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    auto adapter = (*accepted)[std::string_view("adapter")].as_object();
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(
        (*adapter)[std::string_view("interfacePath")].as_string().value_or(""),
        missing_path);

    auto diagnostics =
        (*obj)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());
    EXPECT_FALSE(diagnostics->empty());

    auto document = (*obj)[std::string_view("document")];
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "adapter.interfacePath"));
    EXPECT_FALSE(ArrayHasFieldWithValuePath(document, "pi.controllerName"));
}

TEST_F(MaapiAuthoringFixture, ApplyPathSuccessRefreshesDocument)
{
    auto session = CreateSession();
    auto interface_path = FixturePath("interface_authoring.jsonc").generic_string();
    auto change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"adapter.interfacePath\",\"value\":\""
        + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("sourceFingerprint")]
            .as_string()
            .value_or(""),
        "AuthoringFixture:1.0");

    auto document = (*obj)[std::string_view("document")];
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.controllerName"));
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.resourceName"));
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.options.stage"));
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.options.retry"));
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.options.use-medicine"));
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.options.notify"));
}

TEST_F(MaapiAuthoringFixture, PresetAndOrphansProjectIntoSettings)
{
    auto interface_path = FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"orphanPaths\":[\"pi.tasks:MissingTask\"]}}");

    auto preset = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"applyPreset\",\"payload\":{"
        "\"presetName\":\"DailyPreset\"}}");
    DasPtr<Das::ExportInterface::IDasJson> result_json;
    ASSERT_EQ(session->ApplyChange(preset.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());

    auto accepted =
        (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());
    EXPECT_EQ(
        (*pi)[std::string_view("presetName")].as_string().value_or(""),
        "DailyPreset");
    auto tasks = (*pi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    auto first_task = (*tasks)[0].as_object();
    ASSERT_TRUE(first_task.has_value());
    EXPECT_EQ(
        (*first_task)[std::string_view("taskName")].as_string().value_or(""),
        "DailyFarm");

    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    auto migration =
        (*document)[std::string_view("migration")].as_object();
    ASSERT_TRUE(migration.has_value());
    auto orphans =
        (*migration)[std::string_view("orphanPaths")].as_array();
    ASSERT_TRUE(orphans.has_value());
    ASSERT_EQ(orphans->size(), 1u);
    EXPECT_EQ((*orphans)[0].as_string().value_or(""), "pi.tasks:MissingTask");
}
