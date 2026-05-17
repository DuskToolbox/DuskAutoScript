#include "FakeMaaApiBoundary.h"

#include "../src/MaapiRunTaskComponent.h"
#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using namespace Das;
    using namespace Das::Core::ForeignInterfaceHost;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;

    constexpr std::string_view kRepositoryInvokeComponentGuidText =
        "68F10007-0000-4000-8000-000000000007";

    std::filesystem::path PluginRootDir()
    {
        return std::filesystem::path{IpcTestConfig::GetPluginDir()};
    }

    std::filesystem::path PluginPackageDir()
    {
        return PluginRootDir() / "DasMaaPi";
    }

    std::filesystem::path FlowControlManifestPath()
    {
        const auto package_manifest =
            PluginRootDir() / "DasFlowControl" / "DasFlowControl.json";
        if (std::filesystem::exists(package_manifest))
        {
            return package_manifest;
        }
        return PluginRootDir() / "DasFlowControl.json";
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

    DasGuid GuidFromText(std::string_view guid_text)
    {
        return MakeDasGuid(std::string{guid_text});
    }

    std::string JsonStringLiteral(std::string_view value)
    {
        std::string result = "\"";
        for (const char ch : value)
        {
            switch (ch)
            {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += ch;
                break;
            }
        }
        result += "\"";
        return result;
    }

    std::string SetInterfacePathChangeJson(
        std::string_view interface_path,
        int64_t          base_revision)
    {
        return "{\"baseRevision\":" + std::to_string(base_revision)
               + ",\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
                 "\"adapter.interfacePath\",\"value\":"
               + JsonStringLiteral(interface_path) + "}}";
    }

    DasPtr<Das::ExportInterface::IDasJson> ParseDasJson(std::string json)
    {
        DasPtr<Das::ExportInterface::IDasJson> result;
        EXPECT_EQ(ParseDasJsonFromString(json.c_str(), result.Put()), DAS_S_OK);
        return result;
    }

    yyjson::value ParseYyjson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
    }

    yyjson::value CloneJsonValue(const yyjson::value& value)
    {
        auto text = value.write(yyjson::WriteFlag::NoFlag);
        return ParseYyjson(std::string_view(text.data(), text.size()));
    }

    std::string SerializeJson(const yyjson::value& value)
    {
        auto text = value.write(yyjson::WriteFlag::NoFlag);
        return std::string(text.data(), text.size());
    }

    DasPtr<Das::ExportInterface::IDasJson> WrapYyjson(yyjson::value value)
    {
        return ParseDasJson(SerializeJson(value));
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

    yyjson::value MakeRepositoryCreateRequest()
    {
        yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto          obj = request.as_object();
        (*obj)[std::string_view("pluginGuid")] = std::string(kPluginGuidText);
        (*obj)[std::string_view("taskTypeGuid")] = std::string(kTaskGuidText);
        (*obj)[std::string_view("displayName")] = "Repository invoke MAAPI run";
        return request;
    }

    yyjson::value MakeRepositoryInvokeInput(
        const Das::Core::TaskScheduler::RepositoryInvoke::Dto::
            ChildExecutionSnapshotDto& snapshot)
    {
        Das::Core::TaskScheduler::RepositoryInvoke::Dto::
            ChildExecutionSnapshotDto snapshot_copy;
        snapshot_copy.version = snapshot.version;
        snapshot_copy.source_entry_id = snapshot.source_entry_id;
        snapshot_copy.source_revision = snapshot.source_revision;
        snapshot_copy.source_fingerprint = snapshot.source_fingerprint;
        snapshot_copy.plugin_guid = snapshot.plugin_guid;
        snapshot_copy.task_type_guid = snapshot.task_type_guid;
        snapshot_copy.component_guid = snapshot.component_guid;
        snapshot_copy.execution_input =
            CloneJsonValue(snapshot.execution_input);

        Das::Core::TaskScheduler::RepositoryInvoke::Dto::
            InvokeRepositoryTaskInputDto input;
        input.compiled_snapshot = std::move(snapshot_copy);
        input.runtime_inputs = Das::Utils::MakeYyjsonObject();
        return CloneJsonValue(yyjson::object(input));
    }

    Das::Core::ForeignInterfaceHost::TaskComponentsManifestDesc
    MakeLocalMaapiRunManifest()
    {
        Das::Core::ForeignInterfaceHost::TaskComponentsManifestDesc manifest;
        manifest.factories = std::vector<std::string>{
            std::string(kRunTaskComponentFactoryGuidText)};

        yyjson::value definition(Das::Utils::MakeYyjsonObject());
        auto          definition_obj = definition.as_object();
        (*definition_obj)[std::string_view("schemaVersion")] = 1;
        (*definition_obj)[std::string_view("kind")] = "das.maapi.run";
        (*definition_obj)[std::string_view("componentGuid")] =
            std::string(kRunTaskComponentGuidText);
        (*definition_obj)[std::string_view("settings")] =
            Das::Utils::MakeYyjsonArray();
        (*definition_obj)[std::string_view("inputs")] =
            Das::Utils::MakeYyjsonArray();
        (*definition_obj)[std::string_view("outputs")] =
            Das::Utils::MakeYyjsonArray();
        (*definition_obj)[std::string_view("signals")] =
            Das::Utils::MakeYyjsonArray();
        (*definition_obj)[std::string_view("config")] =
            Das::Utils::MakeYyjsonObject();
        (*definition_obj)[std::string_view("diagnostics")] =
            Das::Utils::MakeYyjsonArray();

        Das::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc entry;
        entry.factory_guid = std::string(kRunTaskComponentFactoryGuidText);
        entry.definition = std::move(definition);

        std::unordered_map<
            std::string,
            Das::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc>
            components;
        components.emplace(
            std::string(kRunTaskComponentGuidText),
            std::move(entry));
        manifest.components = std::move(components);
        return manifest;
    }

    class ChildRequestedStopToken final
        : public PluginInterface::DasStopTokenImplBase<ChildRequestedStopToken>
    {
    public:
        DasResult StopRequested(bool* p_out_stop_requested) override
        {
            if (p_out_stop_requested == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_stop_requested = call_count_++ > 0;
            return DAS_S_OK;
        }

    private:
        int call_count_ = 0;
    };

    class ScopedBoundaryHook final
    {
    public:
        explicit ScopedBoundaryHook(FakeMaaApiBoundary& boundary)
        {
            SetMaaApiBoundaryForTest(&boundary);
        }

        ~ScopedBoundaryHook() { SetMaaApiBoundaryForTest(nullptr); }
    };

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
            ASSERT_TRUE(
                std::filesystem::exists(PluginPackageDir() / "DasMaaPi.json"));
            ASSERT_TRUE(
                std::filesystem::exists(
                    PluginPackageDir() / "libDasMaaPi.dll"));
            ASSERT_TRUE(
                std::filesystem::exists(
                    PluginPackageDir() / "MaaFramework.dll"));
            ASSERT_EQ(
                plugin_manager_->LoadPlugin(PluginPackageDir()),
                DAS_S_OK);
            ASSERT_EQ(
                plugin_manager_->RegisterPluginObjects(PluginPackageDir()),
                DAS_S_OK);
        }

        void TearDown() override
        {
            if (plugin_manager_)
            {
                plugin_manager_->Shutdown();
            }
            std::error_code ec;
            std::filesystem::remove_all(settings_dir_, ec);
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
                    DasIidOf<Das::PluginInterface::
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

    class DasMaaPiRepositoryInvokeFixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            settings_dir_ = UniqueSettingsDir();
            settings_manager_ =
                std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                    settings_dir_);
            ipc_sp_ =
                Das::Core::IPC::MainProcess::CreateIpcContextShared(false);
            scheduler_plugin_manager_ = std::make_unique<PluginManager>(
                *settings_manager_,
                Das::DasSharedRef<Das::Core::IPC::MainProcess::IIpcContext>(
                    ipc_sp_));
            auto scheduler_runtime = CreateCppRuntime();
            ASSERT_NE(scheduler_runtime, nullptr);
            ASSERT_EQ(
                scheduler_plugin_manager_->Initialize(1, scheduler_runtime),
                DAS_S_OK);
            scheduler_registry_ =
                std::make_unique<Das::Core::IPC::RemoteObjectRegistry>();
            scheduler_plugin_manager_->SetRegistry(*scheduler_registry_);
            scheduler_plugin_dir_ = settings_dir_ / "plugins";
            std::filesystem::create_directories(scheduler_plugin_dir_);
            const auto scheduler_maapi_dir = scheduler_plugin_dir_ / "DasMaaPi";
            std::filesystem::copy(
                PluginPackageDir(),
                scheduler_maapi_dir,
                std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::overwrite_existing);
            ASSERT_TRUE(
                std::filesystem::exists(scheduler_maapi_dir / "DasMaaPi.json"));
            scheduler_ =
                std::make_unique<Das::Core::TaskScheduler::SchedulerService>(
                    *scheduler_plugin_manager_,
                    Das::DasSharedRef<Das::Core::IPC::MainProcess::IIpcContext>(
                        ipc_sp_));
            ASSERT_EQ(
                scheduler_->Initialize(scheduler_plugin_dir_, {}),
                DAS_S_OK);

            runtime_plugin_manager_ = std::make_unique<PluginManager>(
                *settings_manager_,
                Das::DasSharedRef<Das::Core::IPC::MainProcess::IIpcContext>(
                    ipc_sp_));
            auto component_runtime = CreateCppRuntime();
            ASSERT_NE(component_runtime, nullptr);
            ASSERT_EQ(
                runtime_plugin_manager_->Initialize(1, component_runtime),
                DAS_S_OK);
            runtime_registry_ =
                std::make_unique<Das::Core::IPC::RemoteObjectRegistry>();
            runtime_plugin_manager_->SetRegistry(*runtime_registry_);
            ASSERT_TRUE(std::filesystem::exists(FlowControlManifestPath()))
                << FlowControlManifestPath().string();
            ASSERT_EQ(
                runtime_plugin_manager_->LoadPlugin(FlowControlManifestPath()),
                DAS_S_OK);
            ASSERT_EQ(
                runtime_plugin_manager_->RegisterPluginObjects(
                    FlowControlManifestPath()),
                DAS_S_OK);
            RegisterLocalMaapiRunFactory();
        }

        void TearDown() override
        {
            if (runtime_plugin_manager_)
            {
                runtime_plugin_manager_->Shutdown();
            }
            runtime_plugin_manager_.reset();
            scheduler_.reset();
            if (scheduler_plugin_manager_)
            {
                scheduler_plugin_manager_->Shutdown();
            }
            scheduler_plugin_manager_.reset();
            runtime_registry_.reset();
            scheduler_registry_.reset();
            settings_manager_.reset();
            std::error_code ec;
            std::filesystem::remove_all(settings_dir_, ec);
        }

        void RegisterLocalMaapiRunFactory()
        {
            auto* factory = new MaapiRunTaskComponentFactory();
            factory->AddRef();

            Das::Core::ForeignInterfaceHost::FeatureInfo feature{};
            feature.feature_type = DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
            feature.interface_ptr = DasPtr<IDasBase>(static_cast<IDasBase*>(
                static_cast<Das::PluginInterface::IDasTaskComponentFactory*>(
                    factory)));

            Das::Core::ForeignInterfaceHost::FeatureInfo* feature_ptr =
                &feature;
            ASSERT_EQ(
                runtime_plugin_manager_->GetTaskComponentFactoryManager()
                    .OnPluginLoaded(
                        GuidFromText(kPluginGuidText),
                        {&feature_ptr, 1},
                        MakeLocalMaapiRunManifest()),
                DAS_S_OK);
        }

        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> CreateSession()
        {
            auto features = scheduler_plugin_manager_->GetFeaturesByType(
                DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);
            EXPECT_EQ(features.size(), 1u);
            DasPtr<Das::PluginInterface::IDasTaskAuthoringSessionFactory>
                factory;
            EXPECT_EQ(
                features[0]->interface_ptr->QueryInterface(
                    DasIidOf<Das::PluginInterface::
                                 IDasTaskAuthoringSessionFactory>(),
                    reinterpret_cast<void**>(factory.Put())),
                DAS_S_OK);
            DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
            EXPECT_EQ(
                factory->CreateSession(
                    GuidFromText(kTaskGuidText),
                    nullptr,
                    session.Put()),
                DAS_S_OK);
            return session;
        }

        yyjson::value CompileDirectExecutionInput(
            std::string_view interface_path)
        {
            auto session = CreateSession();
            auto change =
                ParseDasJson(SetInterfacePathChangeJson(interface_path, 0));
            DasPtr<Das::ExportInterface::IDasJson> apply_json;
            EXPECT_EQ(
                session->ApplyChange(change.Get(), apply_json.Put()),
                DAS_S_OK);

            auto request = ParseDasJson(R"({"purpose":"execution"})");
            DasPtr<Das::ExportInterface::IDasJson> execution_json;
            EXPECT_EQ(
                session->Compile(request.Get(), execution_json.Put()),
                DAS_S_OK);
            auto execution = ReadJsonInterface(execution_json.Get());
            auto execution_obj = execution.as_object();
            EXPECT_TRUE(execution_obj.has_value());
            auto execution_input =
                (*execution_obj)[std::string_view("executionInput")];
            EXPECT_TRUE(execution_input.is_object());
            return CloneJsonValue(execution_input);
        }

        int64_t CreateRepositoryEntryForInterface(
            std::string_view interface_path)
        {
            auto created = scheduler_->CreateRepositoryEntry(
                MakeRepositoryCreateRequest());
            auto created_obj = created.as_object();
            EXPECT_TRUE(created_obj.has_value());
            const auto entry_id =
                (*created_obj)[std::string_view("entryId")].as_sint().value_or(
                    -1);
            EXPECT_GE(entry_id, 0);

            auto change =
                ParseYyjson(SetInterfacePathChangeJson(interface_path, 0));
            auto applied = scheduler_->ApplyRepositoryEntryAuthoringChange(
                entry_id,
                change);
            auto applied_obj = applied.as_object();
            EXPECT_TRUE(applied_obj.has_value());
            EXPECT_TRUE(
                (*applied_obj)[std::string_view("ok")].as_bool().value_or(
                    false));
            EXPECT_EQ(
                (*applied_obj)[std::string_view("revision")].as_sint().value_or(
                    -1),
                1);
            auto authoring =
                (*applied_obj)[std::string_view("authoring")].as_object();
            EXPECT_TRUE(authoring.has_value());
            EXPECT_EQ(
                (*authoring)[std::string_view("sourceFingerprint")]
                    .as_string()
                    .value_or(""),
                std::string_view("SampleInterface:1.0.0"));
            return entry_id;
        }

        Das::Core::TaskScheduler::RepositoryInvoke::
            RepositoryInvokeCompileResult
            CompileRepositoryInvokeSnapshot(int64_t entry_id)
        {
            Das::Core::TaskScheduler::RepositoryInvoke::Dto::
                RepositoryTaskRefDto ref;
            ref.entry_id = entry_id;
            ref.expected_revision = 1;
            ref.source_fingerprint = "SampleInterface:1.0.0";

            Das::Core::TaskScheduler::RepositoryInvoke::
                RepositoryInvokeSourceContext context;
            context.source_entry_id = 200;
            context.source_graph = ParseYyjson(
                std::string("{\"nodes\":[{\"id\":\"maapi-run-node\",")
                + "\"label\":\"MAAPI repository run\",\"settings\":{"
                  "\"repositoryRef\":{\"kind\":\"taskRepositoryRef\","
                  "\"entryId\":"
                + std::to_string(entry_id)
                + ",\"expectedRevision\":1,\"sourceFingerprint\":"
                  "\"SampleInterface:1.0.0\"}}}]}");

            return scheduler_->ResolveRepositoryInvokeSnapshot(ref, context);
        }

        DasPtr<Das::PluginInterface::IDasTaskComponent> CreateRuntimeComponent(
            std::string_view guid_text)
        {
            DasPtr<Das::PluginInterface::IDasTaskComponent> component;
            EXPECT_EQ(
                runtime_plugin_manager_->GetTaskComponentFactoryManager()
                    .CreateComponent(GuidFromText(guid_text), component.Put()),
                DAS_S_OK);
            EXPECT_NE(component.Get(), nullptr);
            return component;
        }

        std::filesystem::path settings_dir_;
        std::filesystem::path scheduler_plugin_dir_;
        std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
            settings_manager_;
        std::shared_ptr<Das::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
        std::unique_ptr<Das::Core::IPC::RemoteObjectRegistry>
            scheduler_registry_;
        std::unique_ptr<Das::Core::IPC::RemoteObjectRegistry> runtime_registry_;
        std::unique_ptr<PluginManager> scheduler_plugin_manager_;
        std::unique_ptr<PluginManager> runtime_plugin_manager_;
        std::unique_ptr<Das::Core::TaskScheduler::SchedulerService> scheduler_;
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

    auto interface_path =
        FixturePath("sample_interface.jsonc").generic_string();
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
    ASSERT_EQ(
        session->Compile(preview_request.Get(), preview_json.Put()),
        DAS_S_OK);
    auto preview = ReadJsonInterface(preview_json.Get());
    EXPECT_TRUE((*preview.as_object())[std::string_view("canExecute")]
                    .as_bool()
                    .value_or(false));
    EXPECT_FALSE(
        preview.as_object()->contains(std::string_view("executionInput")));

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
    EXPECT_TRUE(fake.Contains(
        "PostTask:StartDaily:{\"StartDaily\":{\"enabled\":true}}"));
}

TEST_F(
    DasMaaPiRepositoryInvokeFixture,
    RepositoryInvokeGraphCompileProducesMaapiRunSnapshot)
{
    const auto interface_path =
        FixturePath("sample_interface.jsonc").generic_string();
    const auto direct_execution_input =
        CompileDirectExecutionInput(interface_path);
    const auto entry_id = CreateRepositoryEntryForInterface(interface_path);

    auto result = CompileRepositoryInvokeSnapshot(entry_id);

    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.snapshot.has_value());
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(result.snapshot->source_entry_id, entry_id);
    EXPECT_EQ(result.snapshot->source_revision, 1);
    EXPECT_EQ(
        result.snapshot->source_fingerprint.value_or(""),
        std::string_view("SampleInterface:1.0.0"));
    EXPECT_EQ(result.snapshot->plugin_guid, std::string(kPluginGuidText));
    EXPECT_EQ(result.snapshot->task_type_guid, std::string(kTaskGuidText));
    EXPECT_EQ(
        result.snapshot->component_guid,
        std::string(kRunTaskComponentGuidText));
    EXPECT_EQ(
        SerializeJson(result.snapshot->execution_input),
        SerializeJson(direct_execution_input));

    auto preview = scheduler_->CompileRepositoryEntryAuthoring(
        entry_id,
        ParseYyjson(R"({"purpose":"preview"})"));
    auto preview_obj = preview.as_object();
    ASSERT_TRUE(preview_obj.has_value());
    EXPECT_TRUE(
        (*preview_obj)[std::string_view("canExecute")].as_bool().value_or(
            false));
    EXPECT_FALSE(preview_obj->contains(std::string_view("executionInput")));
    EXPECT_FALSE(preview_obj->contains(std::string_view("compiledSnapshot")));

    auto definitions = runtime_plugin_manager_->GetTaskComponentFactoryManager()
                           .EnumerateDefinitions();
    const auto run_guid = GuidFromText(kRunTaskComponentGuidText);
    auto       found = std::ranges::find_if(
        definitions,
        [&run_guid](const auto& definition)
        { return definition.component_guid == run_guid; });
    ASSERT_NE(found, definitions.end());
    auto definition_obj = found->definition.as_object();
    ASSERT_TRUE(definition_obj.has_value());
    EXPECT_EQ(
        (*definition_obj)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("das.maapi.run"));
}

TEST_F(
    DasMaaPiRepositoryInvokeFixture,
    RepositoryInvokeRuntimeExecutesMaapiRunSnapshotWithoutRepositoryReads)
{
    const auto interface_path =
        FixturePath("sample_interface.jsonc").generic_string();
    const auto entry_id = CreateRepositoryEntryForInterface(interface_path);
    auto       compile = CompileRepositoryInvokeSnapshot(entry_id);
    ASSERT_TRUE(compile.ok);
    ASSERT_TRUE(compile.snapshot.has_value());

    const auto repository_file =
        settings_dir_ / "0"
        / ("taskRepository" + std::to_string(entry_id) + ".json");
    ASSERT_TRUE(std::filesystem::exists(repository_file));
    ASSERT_TRUE(std::filesystem::remove(repository_file));

    FakeMaaApiBoundary fake;
    fake.resource_hash = "sample-hash";
    ScopedBoundaryHook hook(fake);

    auto component = CreateRuntimeComponent(kRepositoryInvokeComponentGuidText);
    auto input = WrapYyjson(MakeRepositoryInvokeInput(*compile.snapshot));
    DasPtr<Das::ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component
            ->Do(nullptr, nullptr, nullptr, input.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto result_obj = result.as_object();
    ASSERT_TRUE(result_obj.has_value());
    EXPECT_EQ(
        (*result_obj)[std::string_view("status")].as_string().value_or(""),
        std::string_view("completed"));
    auto outputs = (*result_obj)[std::string_view("outputs")].as_object();
    ASSERT_TRUE(outputs.has_value());
    EXPECT_EQ(
        (*outputs)[std::string_view("childStatus")].as_string().value_or(""),
        std::string_view("completed"));
    auto child_outputs =
        (*outputs)[std::string_view("childOutputs")].as_object();
    ASSERT_TRUE(child_outputs.has_value());
    auto completed_tasks =
        (*child_outputs)[std::string_view("completedTasks")].as_array();
    ASSERT_TRUE(completed_tasks.has_value());
    ASSERT_EQ(completed_tasks->size(), 1u);
    EXPECT_EQ((*completed_tasks)[0].as_string().value_or(""), "Daily");
    auto signals = (*result_obj)[std::string_view("signals")].as_object();
    ASSERT_TRUE(signals.has_value());
    EXPECT_TRUE(
        (*signals)[std::string_view("succeeded")].as_bool().value_or(false));
    EXPECT_TRUE(fake.Contains(
        "PostTask:StartDaily:{\"StartDaily\":{\"enabled\":true}}"));
    EXPECT_FALSE(std::filesystem::exists(repository_file));
}

TEST_F(
    DasMaaPiRepositoryInvokeFixture,
    RepositoryInvokeRuntimeCancellationPropagatesToMaapiRun)
{
    const auto interface_path =
        FixturePath("sample_interface.jsonc").generic_string();
    const auto entry_id = CreateRepositoryEntryForInterface(interface_path);
    auto       compile = CompileRepositoryInvokeSnapshot(entry_id);
    ASSERT_TRUE(compile.ok);
    ASSERT_TRUE(compile.snapshot.has_value());

    FakeMaaApiBoundary fake;
    fake.resource_hash = "sample-hash";
    ScopedBoundaryHook      hook(fake);
    ChildRequestedStopToken stop_token;

    auto component = CreateRuntimeComponent(kRepositoryInvokeComponentGuidText);
    auto input = WrapYyjson(MakeRepositoryInvokeInput(*compile.snapshot));
    DasPtr<Das::ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component
            ->Do(&stop_token, nullptr, nullptr, input.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto result_obj = result.as_object();
    ASSERT_TRUE(result_obj.has_value());
    EXPECT_EQ(
        (*result_obj)[std::string_view("status")].as_string().value_or(""),
        std::string_view("cancelled"));
    auto outputs = (*result_obj)[std::string_view("outputs")].as_object();
    ASSERT_TRUE(outputs.has_value());
    EXPECT_EQ(
        (*outputs)[std::string_view("childStatus")].as_string().value_or(""),
        std::string_view("cancelled"));
    auto child_outputs =
        (*outputs)[std::string_view("childOutputs")].as_object();
    ASSERT_TRUE(child_outputs.has_value());
    EXPECT_TRUE(
        (*child_outputs)[std::string_view("stopped")].as_bool().value_or(
            false));
    auto signals = (*result_obj)[std::string_view("signals")].as_object();
    ASSERT_TRUE(signals.has_value());
    EXPECT_TRUE(
        (*signals)[std::string_view("cancelled")].as_bool().value_or(false));
    EXPECT_TRUE(fake.Contains("PostStop"));
    EXPECT_FALSE(fake.Contains(
        "PostTask:StartDaily:{\"StartDaily\":{\"enabled\":true}}"));
}
