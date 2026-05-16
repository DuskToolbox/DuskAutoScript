#include "FakeMaaApiBoundary.h"

#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
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
    using namespace Das::Plugins::DasMaaPi::Test;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY;

    std::filesystem::path PluginPackageDir()
    {
        return std::filesystem::path{IpcTestConfig::GetPluginDir()}
               / "DasMaaPi";
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

    std::filesystem::path UniqueSettingsDir()
    {
        const auto ticks =
            std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path()
               / ("das_maapi_integration_" + std::to_string(ticks));
    }

    class MaapiIntegrationFixture : public ::testing::Test
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
            ASSERT_TRUE(std::filesystem::exists(
                PluginPackageDir() / "DasMaaPi.json"));
            ASSERT_TRUE(std::filesystem::exists(
                PluginPackageDir() / "libDasMaaPi.dll"));
            ASSERT_TRUE(std::filesystem::exists(
                PluginPackageDir() / "MaaFramework.dll"));
            ASSERT_EQ(
                plugin_manager_->LoadPlugin(PluginPackageDir()),
                DAS_S_OK);
            ASSERT_EQ(
                plugin_manager_->RegisterPluginObjects(
                    PluginPackageDir()),
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

        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> CreateSession()
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
            DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
            EXPECT_EQ(
                factory->CreateSession(
                    MakeDasGuid(std::string(kTaskGuidText)),
                    nullptr,
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

TEST_F(MaapiIntegrationFixture, SampleInterfaceAuthoringCompileRuntimeDryRun)
{
    auto session = CreateSession();

    DasPtr<Das::ExportInterface::IDasJson> initial_document_json;
    ASSERT_EQ(
        session->GetDocument(nullptr, initial_document_json.Put()),
        DAS_S_OK);
    auto initial_document = ReadJsonInterface(initial_document_json.Get());
    EXPECT_EQ(
        (*initial_document.as_object())[std::string_view("kind")]
            .as_string()
            .value_or(""),
        std::string_view("formSequence"));

    auto interface_path = FixturePath("sample_interface.jsonc").generic_string();
    auto change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"adapter.interfacePath\",\"value\":\""
        + interface_path + "\"}}");
    DasPtr<Das::ExportInterface::IDasJson> apply_json;
    ASSERT_EQ(session->ApplyChange(change.Get(), apply_json.Put()), DAS_S_OK);
    auto apply = ReadJsonInterface(apply_json.Get());
    auto apply_obj = apply.as_object();
    ASSERT_TRUE(apply_obj.has_value());
    EXPECT_EQ(
        (*apply_obj)[std::string_view("sourceFingerprint")]
            .as_string()
            .value_or(""),
        std::string_view("SampleInterface:1.0.0"));
    auto document = (*apply_obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    auto catalog = (*document)[std::string_view("catalog")].as_object();
    ASSERT_TRUE(catalog.has_value());

    auto preview_request = ParseDasJson(R"({"purpose":"preview"})");
    DasPtr<Das::ExportInterface::IDasJson> preview_json;
    ASSERT_EQ(session->Compile(preview_request.Get(), preview_json.Put()), DAS_S_OK);
    auto preview = ReadJsonInterface(preview_json.Get());
    EXPECT_TRUE(
        (*preview.as_object())[std::string_view("canExecute")]
            .as_bool()
            .value_or(false));
    EXPECT_FALSE(preview.as_object()->contains(std::string_view("executionInput")));

    auto execution_request = ParseDasJson(R"({"purpose":"execution"})");
    DasPtr<Das::ExportInterface::IDasJson> execution_json;
    ASSERT_EQ(
        session->Compile(execution_request.Get(), execution_json.Put()),
        DAS_S_OK);
    auto execution = ReadJsonInterface(execution_json.Get());
    auto envelope_value =
        (*execution.as_object())[std::string_view("executionInput")];
    auto parsed = ParseExecutionEnvelope(envelope_value);
    ASSERT_EQ(parsed.result, DAS_S_OK);
    ASSERT_EQ(parsed.envelope.maapi.tasks.size(), 1u);
    EXPECT_EQ(parsed.envelope.maapi.tasks.front().entry, "StartDaily");

    FakeMaaApiBoundary fake;
    fake.resource_hash = "sample-hash";
    auto run_result = MaaRuntime::Run(parsed.envelope, fake, nullptr);
    EXPECT_EQ(run_result.das_result, DAS_S_OK);
    EXPECT_TRUE(fake.Contains("PostTask:StartDaily:{\"StartDaily\":{\"enabled\":true}}"));
}
