#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace
{
    using namespace Das;
    using namespace Das::Core::ForeignInterfaceHost;
    using namespace Das::Plugins::DasMaaPi;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;

    constexpr std::string_view kRunTaskComponentKind = "das.maapi.run";
    constexpr std::string_view kAgentRuntimeTaskComponentKind =
        "das.maapi.agentRuntime";

    std::filesystem::path PluginPackageDir()
    {
        return std::filesystem::path{IpcTestConfig::GetPluginDir()}
               / "DasMaaPi";
    }

    std::filesystem::path MaapiManifestPath()
    {
        return PluginPackageDir() / "DasMaaPi.json";
    }

    std::filesystem::path MaapiLibPath()
    {
        return PluginPackageDir() / "libDasMaaPi.dll";
    }

    std::filesystem::path MaaFrameworkDllPath()
    {
        return PluginPackageDir() / "MaaFramework.dll";
    }

    DasPtr<IForeignLanguageRuntime> CreateCppRuntime()
    {
        ForeignLanguageRuntimeFactoryDesc desc{};
        desc.language = ForeignInterfaceLanguage::Cpp;
        auto runtime_result = CreateForeignLanguageRuntime(desc);
        if (!runtime_result.has_value())
        {
            return nullptr;
        }
        return std::move(runtime_result.value());
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
        if (!parsed)
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return std::move(*parsed);
    }

    yyjson::value ReadManifestJson()
    {
        std::ifstream manifest_file(MaapiManifestPath());
        EXPECT_TRUE(manifest_file.is_open());
        const std::string manifest_content(
            (std::istreambuf_iterator<char>(manifest_file)),
            std::istreambuf_iterator<char>());
        auto parsed = Das::Utils::ParseYyjsonFromString(manifest_content);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
    }

    std::filesystem::path UniqueSettingsDir()
    {
        const auto ticks =
            std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path()
               / ("das_maapi_scaffold_" + std::to_string(ticks));
    }

    class MaapiPluginScaffoldFixture : public ::testing::Test
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

            ASSERT_EQ(plugin_manager_->Initialize(1), DAS_S_OK);

            registry_ =
                std::make_unique<Das::Core::IPC::RemoteObjectRegistry>();
            plugin_manager_->SetRegistry(*registry_);
        }

        void TearDown() override
        {
            if (plugin_manager_)
            {
                plugin_manager_->Shutdown();
            }
            std::filesystem::remove_all(settings_dir_);
        }

        std::filesystem::path settings_dir_;
        std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
        std::unique_ptr<Das::Core::IPC::RemoteObjectRegistry> registry_;
        std::unique_ptr<PluginManager>                        plugin_manager_;
    };
} // namespace

TEST(MaapiPluginScaffoldTest, ArtifactsUseOfficialPluginContract)
{
    ASSERT_TRUE(std::filesystem::is_directory(PluginPackageDir()));
    ASSERT_TRUE(std::filesystem::exists(MaapiManifestPath()));
    ASSERT_TRUE(std::filesystem::exists(MaapiLibPath()));
    ASSERT_TRUE(std::filesystem::exists(MaaFrameworkDllPath()));

    std::ifstream manifest_file(MaapiManifestPath());
    ASSERT_TRUE(manifest_file.is_open());
    const std::string manifest_content(
        (std::istreambuf_iterator<char>(manifest_file)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(
        manifest_content.find(R"("name": "libDasMaaPi")"),
        std::string::npos);
    EXPECT_NE(
        manifest_content.find(std::string(kAuthoringFactoryGuidText)),
        std::string::npos);
}

TEST_F(
    MaapiPluginScaffoldFixture,
    PluginManagerLoadsTaskAndAuthoringFactoryFeatures)
{
    ASSERT_EQ(plugin_manager_->LoadPlugin(PluginPackageDir()), DAS_S_OK);
    ASSERT_EQ(
        plugin_manager_->RegisterPluginObjects(PluginPackageDir()),
        DAS_S_OK);

    const auto plugin_guid = MakeDasGuid(std::string(kPluginGuidText));
    auto*      package = plugin_manager_->FindPluginPackageByGuid(plugin_guid);
    ASSERT_NE(package, nullptr);

    const auto task_guid = MakeDasGuid(std::string(kTaskGuidText));
    auto       task_it = package->task_descriptors.find(task_guid);
    ASSERT_NE(task_it, package->task_descriptors.end());
    ASSERT_TRUE(task_it->second.authoring.has_value());
    EXPECT_EQ(
        task_it->second.authoring->factory_guid,
        MakeDasGuid(std::string(kAuthoringFactoryGuidText)));

    auto task_features =
        plugin_manager_->GetFeaturesByType(DAS_PLUGIN_FEATURE_TASK);
    ASSERT_EQ(task_features.size(), 1u);
    DasPtr<Das::PluginInterface::IDasTask> task;
    ASSERT_EQ(
        task_features[0]->interface_ptr->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasTask>(),
            reinterpret_cast<void**>(task.Put())),
        DAS_S_OK);
    DasGuid loaded_task_guid{};
    ASSERT_EQ(task->GetGuid(&loaded_task_guid), DAS_S_OK);
    EXPECT_EQ(loaded_task_guid, task_guid);

    auto authoring_features = plugin_manager_->GetFeaturesByType(
        DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);
    ASSERT_EQ(authoring_features.size(), 1u);
    DasPtr<Das::PluginInterface::IDasTaskAuthoringSessionFactory> factory;
    ASSERT_EQ(
        authoring_features[0]->interface_ptr->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasTaskAuthoringSessionFactory>(),
            reinterpret_cast<void**>(factory.Put())),
        DAS_S_OK);
    DasGuid loaded_factory_guid{};
    ASSERT_EQ(factory->GetGuid(&loaded_factory_guid), DAS_S_OK);
    EXPECT_EQ(
        loaded_factory_guid,
        MakeDasGuid(std::string(kAuthoringFactoryGuidText)));
}

TEST_F(
    MaapiPluginScaffoldFixture,
    ManifestExposesRunTaskComponentAndExecutionLink)
{
    ASSERT_EQ(plugin_manager_->LoadPlugin(PluginPackageDir()), DAS_S_OK);
    ASSERT_EQ(
        plugin_manager_->RegisterPluginObjects(PluginPackageDir()),
        DAS_S_OK);

    const auto plugin_guid = MakeDasGuid(std::string(kPluginGuidText));
    auto*      package = plugin_manager_->FindPluginPackageByGuid(plugin_guid);
    ASSERT_NE(package, nullptr);

    const auto task_guid = MakeDasGuid(std::string(kTaskGuidText));
    auto       task_it = package->task_descriptors.find(task_guid);
    ASSERT_NE(task_it, package->task_descriptors.end());
    ASSERT_TRUE(task_it->second.execution_component.has_value());
    EXPECT_EQ(
        task_it->second.execution_component->component_guid,
        MakeDasGuid(std::string(kRunTaskComponentGuidText)));

    ASSERT_TRUE(package->task_components.has_value());
    ASSERT_TRUE(package->task_components->factories.has_value());
    EXPECT_NE(
        std::find(
            package->task_components->factories->begin(),
            package->task_components->factories->end(),
            std::string(kRunTaskComponentFactoryGuidText)),
        package->task_components->factories->end());

    ASSERT_TRUE(package->task_components->components.has_value());
    auto component_it = package->task_components->components->find(
        std::string(kRunTaskComponentGuidText));
    ASSERT_NE(component_it, package->task_components->components->end());
    ASSERT_TRUE(component_it->second.definition.has_value());
    auto definition = component_it->second.definition->as_object();
    ASSERT_TRUE(definition.has_value());
    EXPECT_EQ(
        (*definition)[std::string_view("kind")].as_string().value_or(""),
        kRunTaskComponentKind);
}

TEST_F(
    MaapiPluginScaffoldFixture,
    ManifestDoesNotExposeAgentRuntimeTaskComponent)
{
    auto              manifest = ReadManifestJson();
    const auto        text = manifest.write(yyjson::WriteFlag::NoFlag);
    const std::string manifest_text(text.data(), text.size());

    EXPECT_EQ(
        manifest_text.find(std::string(kAgentRuntimeTaskComponentKind)),
        std::string::npos);
    EXPECT_EQ(
        manifest_text.find(std::string(kAgentTaskComponentGuidText)),
        std::string::npos);
    EXPECT_EQ(
        manifest_text.find(std::string(kAgentTaskComponentFactoryGuidText)),
        std::string::npos);
}

TEST_F(MaapiPluginScaffoldFixture, TaskComponentFactoryCreatesRunComponent)
{
    ASSERT_EQ(plugin_manager_->LoadPlugin(PluginPackageDir()), DAS_S_OK);
    ASSERT_EQ(
        plugin_manager_->RegisterPluginObjects(PluginPackageDir()),
        DAS_S_OK);

    auto factories = plugin_manager_->GetFeaturesByType(
        DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY);
    ASSERT_EQ(factories.size(), 1u);

    DasPtr<Das::PluginInterface::IDasTaskComponentFactory> factory;
    ASSERT_EQ(
        factories[0]->interface_ptr->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasTaskComponentFactory>(),
            reinterpret_cast<void**>(factory.Put())),
        DAS_S_OK);

    DasPtr<Das::PluginInterface::IDasTaskComponent> component;
    ASSERT_EQ(
        factory->CreateComponent(
            MakeDasGuid(std::string(kRunTaskComponentGuidText)),
            component.Put()),
        DAS_S_OK);

    DasGuid loaded_component_guid{};
    ASSERT_EQ(component->GetGuid(&loaded_component_guid), DAS_S_OK);
    EXPECT_EQ(
        loaded_component_guid,
        MakeDasGuid(std::string(kRunTaskComponentGuidText)));
}

TEST_F(MaapiPluginScaffoldFixture, AdapterOnlyDocumentBeforeProjectSelection)
{
    ASSERT_EQ(plugin_manager_->LoadPlugin(PluginPackageDir()), DAS_S_OK);
    ASSERT_EQ(
        plugin_manager_->RegisterPluginObjects(PluginPackageDir()),
        DAS_S_OK);

    auto authoring_features = plugin_manager_->GetFeaturesByType(
        DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);
    ASSERT_EQ(authoring_features.size(), 1u);

    DasPtr<Das::PluginInterface::IDasTaskAuthoringSessionFactory> factory;
    ASSERT_EQ(
        authoring_features[0]->interface_ptr->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasTaskAuthoringSessionFactory>(),
            reinterpret_cast<void**>(factory.Put())),
        DAS_S_OK);

    DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
    ASSERT_EQ(
        factory->CreateSession(
            MakeDasGuid(std::string(kTaskGuidText)),
            nullptr,
            session.Put()),
        DAS_S_OK);

    DasPtr<Das::ExportInterface::IDasJson> document_json;
    ASSERT_EQ(session->GetDocument(nullptr, document_json.Put()), DAS_S_OK);
    auto document = ReadJsonInterface(document_json.Get());
    auto obj = document.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("formSequence"));

    auto values = (*obj)[std::string_view("values")].as_object();
    ASSERT_TRUE(values.has_value());
    auto adapter = (*values)[std::string_view("adapter")].as_object();
    ASSERT_TRUE(adapter.has_value());
    auto execution_policy =
        (*adapter)[std::string_view("executionPolicy")].as_object();
    ASSERT_TRUE(execution_policy.has_value());
    EXPECT_TRUE(
        (*execution_policy)[std::string_view("failFast")].as_bool().value_or(
            false));

    const auto        serialized = document.write(yyjson::WriteFlag::NoFlag);
    const std::string text(serialized.data(), serialized.size());
    EXPECT_EQ(text.find("controller"), std::string::npos);
    EXPECT_EQ(text.find("resource"), std::string::npos);
    EXPECT_EQ(text.find("preset"), std::string::npos);
}
