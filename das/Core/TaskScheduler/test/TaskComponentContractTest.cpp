#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>

namespace
{
    using Das::DasPtr;
    using Das::Core::ForeignInterfaceHost::PluginManager;
    using Das::Core::TaskScheduler::RepositoryInvoke::Dto::
        ChildExecutionSnapshotDto;
    using Das::Core::TaskScheduler::RepositoryInvoke::Dto::
        InvokeRepositoryTaskInputDto;
    using Das::PluginInterface::IDasTaskComponent;

    struct OfficialFlowComponent
    {
        std::string_view name;
        std::string_view kind;
        std::string_view guid_text;
    };

    constexpr std::array<OfficialFlowComponent, 7> kOfficialComponents{
        OfficialFlowComponent{
            "branch",
            "das.flow.branch",
            "68F10001-0000-4000-8000-000000000001"},
        OfficialFlowComponent{
            "sequence",
            "das.flow.sequence",
            "68F10002-0000-4000-8000-000000000002"},
        OfficialFlowComponent{
            "delay",
            "das.flow.delay",
            "68F10003-0000-4000-8000-000000000003"},
        OfficialFlowComponent{
            "for",
            "das.flow.for",
            "68F10004-0000-4000-8000-000000000004"},
        OfficialFlowComponent{
            "while",
            "das.flow.while",
            "68F10005-0000-4000-8000-000000000005"},
        OfficialFlowComponent{
            "goto",
            "das.flow.goto",
            "68F10006-0000-4000-8000-000000000006"},
        OfficialFlowComponent{
            "repositoryInvoke",
            "das.flow.invokeRepository",
            "68F10007-0000-4000-8000-000000000007"},
    };

    constexpr std::string_view kRepositoryInvokeComponentGuid =
        "68F10007-0000-4000-8000-000000000007";
    constexpr std::string_view kFakeChildPluginGuid =
        "68F19998-0000-4000-8000-000000000001";
    constexpr std::string_view kFakeChildFactoryGuid =
        "68F19998-0000-4000-8000-000000000002";
    constexpr std::string_view kFakeChildComponentGuid =
        "68F19999-0000-4000-8000-000000000001";
    constexpr std::string_view kMissingChildComponentGuid =
        "68F19999-0000-4000-8000-000000000099";
    constexpr std::string_view kMaapiRunTaskComponentGuid =
        "69F20008-0000-4000-8000-000000000001";

    std::filesystem::path UniqueSettingsDir()
    {
        static std::atomic<int> counter{0};
        std::random_device      rd;
        return std::filesystem::current_path()
               / ("task_component_contract_settings_" + std::to_string(rd())
                  + "_" + std::to_string(counter.fetch_add(1)));
    }

    DasGuid GuidFromText(std::string_view guid_text)
    {
        return Das::Core::ForeignInterfaceHost::MakeDasGuid(
            std::string{guid_text});
    }

    DasPtr<Das::Core::ForeignInterfaceHost::IForeignLanguageRuntime>
    CreateCppRuntime()
    {
        Das::Core::ForeignInterfaceHost::ForeignLanguageRuntimeFactoryDesc
            desc{};
        desc.language =
            Das::Core::ForeignInterfaceHost::ForeignInterfaceLanguage::Cpp;
        auto result =
            Das::Core::ForeignInterfaceHost::CreateForeignLanguageRuntime(desc);
        if (!result)
        {
            return nullptr;
        }
        return std::move(result.value());
    }

    std::filesystem::path FindDasFlowControlManifest()
    {
        if (const char* plugin_dir = std::getenv("DAS_PLUGIN_DIR");
            plugin_dir != nullptr && plugin_dir[0] != '\0')
        {
            return std::filesystem::path{plugin_dir} / "DasFlowControl.json";
        }

        const auto cwd = std::filesystem::current_path();
        const std::array<std::filesystem::path, 3> candidates{
            cwd / "plugins" / "DasFlowControl.json",
            cwd.parent_path() / "bin" / "Debug" / "plugins"
                / "DasFlowControl.json",
            cwd / "build" / "mingw-debug" / "bin" / "Debug" / "plugins"
                / "DasFlowControl.json",
        };
        const auto found = std::ranges::find_if(
            candidates,
            [](const std::filesystem::path& candidate)
            { return std::filesystem::exists(candidate); });
        return found != candidates.end()
                   ? *found
                   : std::filesystem::path{"DasFlowControl.json"};
    }

    std::filesystem::path SourceRoot()
    {
        auto path = std::filesystem::path(__FILE__);
        for (int i = 0; i < 5; ++i)
        {
            path = path.parent_path();
        }
        return path;
    }

    std::string ReadFileText(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        EXPECT_TRUE(input.is_open()) << path.string();
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    yyjson::value ToJson(Das::ExportInterface::IDasJson* json)
    {
        DasPtr<IDasReadOnlyString> text;
        EXPECT_EQ(json->ToString(0, text.Put()), DAS_S_OK);
        const char* u8 = nullptr;
        EXPECT_EQ(text->GetUtf8(&u8), DAS_S_OK);
        auto parsed =
            Das::Utils::ParseYyjsonFromString(u8 != nullptr ? u8 : "");
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    std::string ToJsonText(Das::ExportInterface::IDasJson* json)
    {
        if (json == nullptr)
        {
            return {};
        }

        DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return {};
        }

        const char* u8 = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&u8)) || u8 == nullptr)
        {
            return {};
        }
        return std::string{u8};
    }

    DasPtr<Das::ExportInterface::IDasJson> Wrap(yyjson::value value)
    {
        return Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
            std::move(value));
    }

    template <typename ObjectRef>
    std::string_view StringField(const ObjectRef& object, std::string_view key)
    {
        return object[key].as_string().value_or("");
    }

    yyjson::value ParseJson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    yyjson::value MakeDefinition(
        std::string_view component_guid,
        std::string_view kind)
    {
        auto definition = Das::Utils::MakeYyjsonObject();
        auto obj = *definition.as_object();
        obj[std::string_view("schemaVersion")] = 1;
        obj[std::string_view("componentGuid")] = component_guid;
        obj[std::string_view("kind")] = kind;
        obj[std::string_view("inputs")] = Das::Utils::MakeYyjsonArray();
        obj[std::string_view("outputs")] = Das::Utils::MakeYyjsonArray();
        obj[std::string_view("config")] = Das::Utils::MakeYyjsonObject();
        obj[std::string_view("diagnostics")] = Das::Utils::MakeYyjsonArray();
        return definition;
    }

    Das::Core::ForeignInterfaceHost::TaskComponentsManifestDesc
    MakeChildManifest()
    {
        Das::Core::ForeignInterfaceHost::TaskComponentsManifestDesc manifest;
        manifest.factories =
            std::vector<std::string>{std::string{kFakeChildFactoryGuid}};

        Das::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc entry;
        entry.factory_guid = std::string{kFakeChildFactoryGuid};
        entry.definition =
            MakeDefinition(kFakeChildComponentGuid, "test.child.recording");

        std::unordered_map<
            std::string,
            Das::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc>
            components;
        components.emplace(
            std::string{kFakeChildComponentGuid},
            std::move(entry));
        manifest.components = std::move(components);
        return manifest;
    }

    yyjson::value MakeRepositoryInvokeInput(
        std::string_view component_guid,
        int32_t          snapshot_version = 1)
    {
        ChildExecutionSnapshotDto snapshot;
        snapshot.version = snapshot_version;
        snapshot.source_entry_id = 42;
        snapshot.source_revision = 7;
        snapshot.source_fingerprint = "source-hash";
        snapshot.plugin_guid = std::string{kFakeChildPluginGuid};
        snapshot.task_type_guid = "68F19997-0000-4000-8000-000000000001";
        snapshot.component_guid = std::string{component_guid};
        snapshot.execution_input = ParseJson(R"json({
            "childInput": "from-snapshot",
            "providerPrivate": {"exactKey": true}
        })json");

        InvokeRepositoryTaskInputDto input;
        input.compiled_snapshot = std::move(snapshot);
        input.runtime_inputs = ParseJson(R"json({"callerValue": 9})json");

        auto serialized = yyjson::object(input);
        auto text = serialized.write(yyjson::WriteFlag::NoFlag);
        auto parsed = Das::Utils::ParseYyjsonFromString(
            std::string_view(text.data(), text.size()));
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    std::string_view DiagnosticCode(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj)
        {
            return {};
        }
        auto diagnostics = (*obj)[std::string_view("diagnostics")].as_array();
        if (!diagnostics || diagnostics->empty())
        {
            return {};
        }
        auto diagnostic = (*diagnostics)[0].as_object();
        if (!diagnostic)
        {
            return {};
        }
        return (*diagnostic)[std::string_view("code")].as_string().value_or("");
    }

    void TestSetPortString(
        Das::ExportInterface::IDasPortMap* map,
        std::string_view                   key,
        std::string_view                   value)
    {
        DasReadOnlyString k{std::string{key}.c_str()};
        DasReadOnlyString v{std::string{value}.c_str()};
        map->SetString(k.Get(), v.Get());
    }

    void TestSetPortBool(
        Das::ExportInterface::IDasPortMap* map,
        std::string_view                   key,
        bool                               value)
    {
        DasReadOnlyString k{std::string{key}.c_str()};
        map->SetBool(k.Get(), value);
    }

    std::string TestGetPortString(
        Das::ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                           key)
    {
        IDasReadOnlyString* p_str = nullptr;
        DasReadOnlyString   k{std::string{key}.c_str()};
        auto                hr = map->GetString(k.Get(), &p_str);
        if (DAS::IsFailed(hr) || !p_str)
        {
            return {};
        }
        const char* u8 = nullptr;
        p_str->GetUtf8(&u8);
        std::string result(u8 ? u8 : "");
        p_str->Release();
        return result;
    }

    std::optional<yyjson::value> TestGetPortJson(
        Das::ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                           key)
    {
        auto str = TestGetPortString(map, key);
        if (str.empty())
        {
            return std::nullopt;
        }
        return Das::Utils::ParseYyjsonFromString(str);
    }

    class FakeStopToken final : public Das::PluginInterface::IDasStopToken
    {
    public:
        explicit FakeStopToken(bool requested = false) : requested_(requested)
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<Das::PluginInterface::IDasStopToken>())
            {
                *pp_out =
                    static_cast<Das::PluginInterface::IDasStopToken*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL StopRequested(bool* p_requested) override
        {
            if (p_requested == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_requested = requested_;
            return DAS_S_OK;
        }

    private:
        bool                  requested_ = false;
        std::atomic<uint32_t> ref_count_{0};
    };

    struct RecordingChildState
    {
        int                                  do_call_count = 0;
        DasResult                            do_result = DAS_S_OK;
        Das::PluginInterface::IDasStopToken* last_stop_token = nullptr;
        std::string                          last_environment_json;
        std::string                          last_settings_json;
        std::string                          last_input_json;
        std::string                          result_json = R"json({
            "status": "completed",
            "outputs": {"value": 5},
            "diagnostics": [],
            "signals": {"succeeded": true}
        })json";
    };

    class RecordingChildTaskComponent final : public IDasTaskComponent
    {
    public:
        RecordingChildTaskComponent(
            DasGuid                              guid,
            std::shared_ptr<RecordingChildState> state)
            : guid_(guid), state_(std::move(state))
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTypeInfo>())
            {
                *pp_out = static_cast<IDasTypeInfo*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTaskComponent>())
            {
                *pp_out = static_cast<IDasTaskComponent*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = guid_;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (pp_out_name == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            return CreateIDasReadOnlyStringFromUtf8(
                "RecordingChildTaskComponent",
                pp_out_name);
        }

        DasResult DAS_STD_CALL ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult DAS_STD_CALL
        Do(Das::PluginInterface::IDasStopToken*       stop_token,
           Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
           Das::ExportInterface::IDasPortMap**        pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            ++state_->do_call_count;
            state_->last_stop_token = stop_token;

            if (p_input_port_map != nullptr)
            {
                state_->last_input_json =
                    TestGetPortString(p_input_port_map, "executionInput");
            }

            if (DAS::IsFailed(state_->do_result))
            {
                return state_->do_result;
            }

            auto result_parsed =
                Das::Utils::ParseYyjsonFromString(state_->result_json);
            if (!result_parsed)
            {
                return DAS_E_FAIL;
            }

            DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
            DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }

            auto result_obj = result_parsed->as_object();
            if (result_obj)
            {
                auto status_val = (*result_obj)[std::string_view("status")]
                                      .as_string()
                                      .value_or("");
                TestSetPortString(output_map.Get(), "status", status_val);

                auto outputs = (*result_obj)[std::string_view("outputs")];
                if (!outputs.is_null())
                {
                    auto serialized = outputs.write(yyjson::WriteFlag::NoFlag);
                    TestSetPortString(
                        output_map.Get(),
                        "childOutputs",
                        std::string_view(serialized));
                }
            }

            *pp_out_port_map = output_map.Get();
            (*pp_out_port_map)->AddRef();
            return DAS_S_OK;
        }

    private:
        DasGuid                              guid_{};
        std::shared_ptr<RecordingChildState> state_;
        std::atomic<uint32_t>                ref_count_{0};
    };

    class RecordingChildTaskComponentFactory final
        : public Das::PluginInterface::IDasTaskComponentFactory
    {
    public:
        explicit RecordingChildTaskComponentFactory(
            std::shared_ptr<RecordingChildState> state)
            : state_(std::move(state))
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTypeInfo>())
            {
                *pp_out = static_cast<IDasTypeInfo*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid
                == DasIidOf<Das::PluginInterface::IDasTaskComponentFactory>())
            {
                *pp_out = static_cast<
                    Das::PluginInterface::IDasTaskComponentFactory*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = GuidFromText(kFakeChildFactoryGuid);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (pp_out_name == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            return CreateIDasReadOnlyStringFromUtf8(
                "RecordingChildTaskComponentFactory",
                pp_out_name);
        }

        DasResult DAS_STD_CALL CreateComponent(
            const DasGuid&      component_guid,
            IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            if (component_guid != GuidFromText(kFakeChildComponentGuid))
            {
                return DAS_E_NOT_FOUND;
            }

            ++create_call_count;
            auto* component =
                new RecordingChildTaskComponent(component_guid, state_);
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        int create_call_count = 0;

        DasResult DAS_STD_CALL SetTaskComponentHost(
            Das::PluginInterface::IDasTaskComponentHost* /*p_host*/) override
        {
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<RecordingChildState> state_;
        std::atomic<uint32_t>                ref_count_{0};
    };

    class TaskComponentContractTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            settings_dir_ = UniqueSettingsDir();
            settings_manager_ =
                std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                    settings_dir_);
            ipc_sp_ =
                DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
            plugin_manager_ = std::make_unique<PluginManager>(
                *settings_manager_,
                Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                    ipc_sp_));

            ASSERT_EQ(plugin_manager_->Initialize(1), DAS_S_OK);

            manifest_path_ = FindDasFlowControlManifest();
            ASSERT_TRUE(std::filesystem::exists(manifest_path_))
                << "DasFlowControl manifest not found at "
                << manifest_path_.string();
            ASSERT_EQ(plugin_manager_->LoadPlugin(manifest_path_), DAS_S_OK);
        }

        void TearDown() override
        {
            if (plugin_manager_)
            {
                plugin_manager_->Shutdown();
            }
            plugin_manager_.reset();
            settings_manager_.reset();
            std::filesystem::remove_all(settings_dir_);
        }

        DasPtr<IDasTaskComponent> CreateComponent(std::string_view guid_text)
        {
            DasPtr<IDasTaskComponent> component;
            EXPECT_EQ(
                plugin_manager_->GetTaskComponentFactoryManager()
                    .CreateComponent(GuidFromText(guid_text), component.Put()),
                DAS_S_OK);
            EXPECT_NE(component.Get(), nullptr);
            return component;
        }

        RecordingChildTaskComponentFactory* RegisterRecordingChildFactory(
            const std::shared_ptr<RecordingChildState>& state)
        {
            auto* factory = new RecordingChildTaskComponentFactory(state);
            factory->AddRef();

            Das::Core::ForeignInterfaceHost::FeatureInfo feature{};
            feature.feature_type =
                Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
            feature.interface_ptr = DasPtr<IDasBase>(static_cast<IDasBase*>(
                static_cast<Das::PluginInterface::IDasTaskComponentFactory*>(
                    factory)));

            Das::Core::ForeignInterfaceHost::FeatureInfo* feature_ptr =
                &feature;
            EXPECT_EQ(
                plugin_manager_->GetTaskComponentFactoryManager()
                    .OnPluginLoaded(
                        GuidFromText(kFakeChildPluginGuid),
                        {&feature_ptr, 1},
                        MakeChildManifest()),
                DAS_S_OK);
            return factory;
        }

        std::filesystem::path settings_dir_;
        std::filesystem::path manifest_path_;
        std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
            settings_manager_;
        std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
        std::unique_ptr<PluginManager> plugin_manager_;
    };

} // namespace

TEST_F(
    TaskComponentContractTest,
    DasFlowControlManifestDefinitionsComeFromManager)
{
    auto definitions = plugin_manager_->GetTaskComponentFactoryManager()
                           .EnumerateDefinitions();
    ASSERT_EQ(definitions.size(), kOfficialComponents.size());

    for (const auto& expected : kOfficialComponents)
    {
        const auto expected_guid = GuidFromText(expected.guid_text);
        const auto found = std::ranges::find_if(
            definitions,
            [&expected_guid](const auto& definition)
            { return definition.component_guid == expected_guid; });
        ASSERT_NE(found, definitions.end()) << expected.name;

        auto obj = found->definition.as_object();
        ASSERT_TRUE(obj.has_value()) << expected.name;
        EXPECT_EQ(StringField(*obj, "componentGuid"), expected.guid_text)
            << expected.name;
        EXPECT_EQ(StringField(*obj, "kind"), expected.kind) << expected.name;
        EXPECT_TRUE((*obj)[std::string_view("inputs")].is_array())
            << expected.name;
        EXPECT_TRUE((*obj)[std::string_view("outputs")].is_array())
            << expected.name;
        EXPECT_TRUE((*obj)[std::string_view("config")].is_object())
            << expected.name;
        EXPECT_TRUE((*obj)[std::string_view("diagnostics")].is_array())
            << expected.name;
    }
}

TEST_F(
    TaskComponentContractTest,
    RepositoryInvokeFinalCatalogHidesMaapiAgentRuntime)
{
    auto       definitions = plugin_manager_->GetTaskComponentFactoryManager()
                                 .EnumerateDefinitions();
    const auto repository_invoke_guid =
        GuidFromText(kRepositoryInvokeComponentGuid);
    auto repository_invoke = std::ranges::find_if(
        definitions,
        [&repository_invoke_guid](const auto& definition)
        { return definition.component_guid == repository_invoke_guid; });
    ASSERT_NE(repository_invoke, definitions.end());
    auto repository_definition = repository_invoke->definition.as_object();
    ASSERT_TRUE(repository_definition.has_value());
    EXPECT_EQ(
        StringField(*repository_definition, "kind"),
        std::string_view("das.flow.invokeRepository"));

    const auto maapi_manifest_path =
        SourceRoot() / "das" / "Plugins" / "DasMaaPi" / "DasMaaPi.json";
    const auto maapi_manifest_text = ReadFileText(maapi_manifest_path);
    EXPECT_EQ(
        maapi_manifest_text.find("das.maapi.agentRuntime"),
        std::string::npos);

    auto maapi_manifest = ParseJson(maapi_manifest_text);
    auto maapi_obj = maapi_manifest.as_object();
    ASSERT_TRUE(maapi_obj.has_value());
    auto task_components =
        (*maapi_obj)[std::string_view("taskComponents")].as_object();
    ASSERT_TRUE(task_components.has_value());
    auto components =
        (*task_components)[std::string_view("components")].as_object();
    ASSERT_TRUE(components.has_value());
    auto run_entry = (*components)[kMaapiRunTaskComponentGuid].as_object();
    ASSERT_TRUE(run_entry.has_value());
    auto definition = (*run_entry)[std::string_view("definition")].as_object();
    ASSERT_TRUE(definition.has_value());
    EXPECT_EQ(
        StringField(*definition, "kind"),
        std::string_view("das.maapi.run"));
}

TEST_F(
    TaskComponentContractTest,
    DasFlowControlFeatureFactoryCreatesManifestComponents)
{
    auto runtime = CreateCppRuntime();
    ASSERT_NE(runtime, nullptr);
    auto plugin_result = runtime->LoadPlugin(manifest_path_);
    ASSERT_TRUE(plugin_result.has_value())
        << "Failed to load DasFlowControl from " << manifest_path_.string();

    auto plugin_base = plugin_result.value();
    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    ASSERT_EQ(plugin_base.As(package.Put()), DAS_S_OK);

    bool     found_factory_feature = false;
    uint64_t task_component_feature_index = 0;
    for (uint64_t index = 0;; ++index)
    {
        Das::PluginInterface::DasPluginFeature feature{};
        if (package->EnumFeature(index, &feature) != DAS_S_OK)
        {
            break;
        }
        if (feature
            == Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY)
        {
            found_factory_feature = true;
            task_component_feature_index = index;
            break;
        }
    }
    ASSERT_TRUE(found_factory_feature);

    IDasBase* factory_base_raw = nullptr;
    ASSERT_EQ(
        package->CreateFeatureInterface(
            task_component_feature_index,
            &factory_base_raw),
        DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DasPtr<IDasBase> factory_base(factory_base_raw);

    DasPtr<Das::PluginInterface::IDasTaskComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    for (const auto& expected : kOfficialComponents)
    {
        DasPtr<Das::PluginInterface::IDasTaskComponent> component;
        EXPECT_EQ(
            factory->CreateComponent(
                GuidFromText(expected.guid_text),
                component.Put()),
            DAS_S_OK)
            << expected.name;
        EXPECT_NE(component.Get(), nullptr) << expected.name;
    }
}

TEST_F(TaskComponentContractTest, CreatesAllOfficialFlowComponentsByGuid)
{
    for (const auto& expected : kOfficialComponents)
    {
        auto component = CreateComponent(expected.guid_text);
        ASSERT_NE(component.Get(), nullptr) << expected.name;

        DasPtr<IDasTypeInfo> type_info;
        EXPECT_EQ(
            component->QueryInterface(
                DasIidOf<IDasTypeInfo>(),
                reinterpret_cast<void**>(type_info.Put())),
            DAS_S_OK)
            << expected.name;
    }
}

TEST_F(TaskComponentContractTest, ApplySettingsChangeReturnsAcceptedSettings)
{
    auto component = CreateComponent(kOfficialComponents[0].guid_text);
    ASSERT_NE(component.Get(), nullptr);

    auto change = Das::Utils::ParseYyjsonFromString(R"json({
        "kind": "setValue",
        "payload": {"path": "label", "value": "choose route"}
    })json");
    ASSERT_TRUE(change.has_value());
    auto request = Wrap(std::move(*change));

    DasPtr<Das::ExportInterface::IDasJson> result;
    ASSERT_EQ(
        component->ApplySettingsChange(request.Get(), result.Put()),
        DAS_S_OK);

    auto json = ToJson(result.Get());
    EXPECT_EQ(
        (*(*json.as_object())[std::string_view("acceptedSettings")]
              .as_object())[std::string_view("label")]
            .as_string()
            .value(),
        "choose route");
}

TEST_F(TaskComponentContractTest, BranchDoReturnsTrueAndFalseSignals)
{
    auto component = CreateComponent(kOfficialComponents[0].guid_text);
    ASSERT_NE(component.Get(), nullptr);

    const std::array<std::pair<bool, std::string_view>, 2> cases{
        std::pair{true, std::string_view{"true"}},
        std::pair{false, std::string_view{"false"}},
    };

    for (const auto& [condition, expected_signal] : cases)
    {
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
        ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
        TestSetPortBool(input_map.Get(), "condition", condition);

        DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
        ASSERT_EQ(
            component->Do(nullptr, input_map.Get(), result_map.Put()),
            DAS_S_OK);

        auto status = TestGetPortString(result_map.Get(), "status");
        EXPECT_EQ(status, "completed");
        auto outputs_json = TestGetPortJson(result_map.Get(), "outputs");
        ASSERT_TRUE(outputs_json.has_value());
        EXPECT_TRUE(outputs_json->is_object());
        auto signals_json = TestGetPortJson(result_map.Get(), "signals");
        ASSERT_TRUE(signals_json.has_value());
        auto signals = signals_json->as_array();
        ASSERT_TRUE(signals.has_value());
        ASSERT_EQ(signals->size(), 1u);
        EXPECT_EQ((*signals)[0].as_string().value(), expected_signal);
    }
}

TEST_F(TaskComponentContractTest, RepositoryInvokeExecutesCompiledSnapshotChild)
{
    auto      state = std::make_shared<RecordingChildState>();
    auto*     child_factory = RegisterRecordingChildFactory(state);
    const int create_count_before = child_factory->create_call_count;

    auto component = CreateComponent(kRepositoryInvokeComponentGuid);
    ASSERT_NE(component.Get(), nullptr);

    FakeStopToken stop_token;
    auto input_json = MakeRepositoryInvokeInput(kFakeChildComponentGuid);
    auto input_obj = input_json.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    TestSetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(&stop_token, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    EXPECT_EQ(child_factory->create_call_count, create_count_before + 1);
    EXPECT_EQ(state->do_call_count, 1);
    EXPECT_EQ(state->last_stop_token, &stop_token);

    auto child_input = ParseJson(state->last_input_json);
    auto child_input_obj = child_input.as_object();
    ASSERT_TRUE(child_input_obj.has_value());
    EXPECT_EQ(
        (*child_input_obj)[std::string_view("childInput")].as_string().value_or(
            ""),
        std::string_view("from-snapshot"));

    auto status = TestGetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "completed");
    auto child_status = TestGetPortString(result_map.Get(), "childStatus");
    EXPECT_EQ(child_status, "completed");
    auto child_outputs_json = TestGetPortJson(result_map.Get(), "childOutputs");
    ASSERT_TRUE(child_outputs_json.has_value());
    auto child_outputs_obj = child_outputs_json->as_object();
    ASSERT_TRUE(child_outputs_obj.has_value());
    EXPECT_EQ(
        (*child_outputs_obj)[std::string_view("value")].as_int().value(),
        5);

    child_factory->Release();
}

TEST_F(TaskComponentContractTest, RepositoryInvokeRequiresCompiledSnapshot)
{
    auto component = CreateComponent(kRepositoryInvokeComponentGuid);
    ASSERT_NE(component.Get(), nullptr);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = TestGetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "failed");
}

TEST_F(TaskComponentContractTest, RepositoryInvokeFailsWhenHostUnavailable)
{
    auto runtime = CreateCppRuntime();
    ASSERT_NE(runtime, nullptr);
    auto plugin_result = runtime->LoadPlugin(manifest_path_);
    ASSERT_TRUE(plugin_result.has_value());

    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    ASSERT_EQ(plugin_result.value().As(package.Put()), DAS_S_OK);

    IDasBase* factory_base_raw = nullptr;
    ASSERT_EQ(package->CreateFeatureInterface(0, &factory_base_raw), DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DasPtr<IDasBase> factory_base(factory_base_raw);

    DasPtr<Das::PluginInterface::IDasTaskComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    DasPtr<IDasTaskComponent> component;
    ASSERT_EQ(
        factory->CreateComponent(
            GuidFromText(kRepositoryInvokeComponentGuid),
            component.Put()),
        DAS_S_OK);

    auto input = MakeRepositoryInvokeInput(kFakeChildComponentGuid);
    auto input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    TestSetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, input_map.Get(), result.Put()), DAS_S_OK);

    auto status = TestGetPortString(result.Get(), "status");
    EXPECT_EQ(status, "failed");
}

TEST_F(TaskComponentContractTest, RepositoryInvokeReportsMissingChildRoute)
{
    auto component = CreateComponent(kRepositoryInvokeComponentGuid);
    ASSERT_NE(component.Get(), nullptr);

    auto input = MakeRepositoryInvokeInput(kMissingChildComponentGuid);
    auto input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    TestSetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = TestGetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "failed");
}

TEST_F(TaskComponentContractTest, RepositoryInvokeReportsChildFailure)
{
    auto state = std::make_shared<RecordingChildState>();
    state->do_result = DAS_E_FAIL;
    auto* child_factory = RegisterRecordingChildFactory(state);

    auto component = CreateComponent(kRepositoryInvokeComponentGuid);
    auto input = MakeRepositoryInvokeInput(kFakeChildComponentGuid);
    auto input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    TestSetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = TestGetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "failed");

    child_factory->Release();
}

TEST_F(TaskComponentContractTest, RepositoryInvokeReportsMalformedChildResult)
{
    auto state = std::make_shared<RecordingChildState>();
    state->result_json = R"json([])json";
    auto* child_factory = RegisterRecordingChildFactory(state);

    auto component = CreateComponent(kRepositoryInvokeComponentGuid);
    auto input = MakeRepositoryInvokeInput(kFakeChildComponentGuid);
    auto input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    TestSetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = TestGetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "failed");

    child_factory->Release();
}

TEST_F(TaskComponentContractTest, RepositoryInvokeRejectsInvalidSnapshotVersion)
{
    auto component = CreateComponent(kRepositoryInvokeComponentGuid);
    ASSERT_NE(component.Get(), nullptr);

    auto input = MakeRepositoryInvokeInput(kFakeChildComponentGuid, 2);
    auto input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    TestSetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = TestGetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "failed");
}

TEST_F(TaskComponentContractTest, RepositoryInvokeHonorsAlreadyRequestedStop)
{
    auto      state = std::make_shared<RecordingChildState>();
    auto*     child_factory = RegisterRecordingChildFactory(state);
    const int create_count_before = child_factory->create_call_count;

    auto          component = CreateComponent(kRepositoryInvokeComponentGuid);
    FakeStopToken stop_token(true);
    auto          input = MakeRepositoryInvokeInput(kFakeChildComponentGuid);
    auto          input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    TestSetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(&stop_token, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = TestGetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "cancelled");
    EXPECT_EQ(child_factory->create_call_count, create_count_before);
    EXPECT_EQ(state->do_call_count, 0);

    child_factory->Release();
}

TEST_F(TaskComponentContractTest, TaskComponentDoesNotExposeDynamicComponent)
{
    auto component = CreateComponent(kOfficialComponents[0].guid_text);
    ASSERT_NE(component.Get(), nullptr);

    void* out = nullptr;
    EXPECT_EQ(
        component->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasComponent>(),
            &out),
        DAS_E_NO_INTERFACE);
    EXPECT_EQ(out, nullptr);
}
