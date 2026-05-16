#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <random>

namespace
{
    using Das::DasPtr;
    using Das::Core::ForeignInterfaceHost::PluginManager;
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

            auto runtime = CreateCppRuntime();
            ASSERT_NE(runtime, nullptr);
            ASSERT_EQ(plugin_manager_->Initialize(1, runtime), DAS_S_OK);

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

        std::filesystem::path settings_dir_;
        std::filesystem::path manifest_path_;
        std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
            settings_manager_;
        std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
        std::unique_ptr<PluginManager> plugin_manager_;
    };

} // namespace

TEST_F(TaskComponentContractTest, DasFlowControlManifestDefinitionsComeFromManager)
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

TEST_F(TaskComponentContractTest, DasFlowControlFeatureFactoryCreatesManifestComponents)
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
            == Das::PluginInterface::
                DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY)
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

    const std::array<std::pair<std::string_view, std::string_view>, 2> cases{
        std::pair{
            std::string_view{R"json({"condition": true})json"},
            std::string_view{"true"}},
        std::pair{
            std::string_view{R"json({"condition": false})json"},
            std::string_view{"false"}},
    };

    for (const auto& [input_text, expected_signal] : cases)
    {
        auto input = Das::Utils::ParseYyjsonFromString(input_text);
        ASSERT_TRUE(input.has_value());
        auto input_json = Wrap(std::move(*input));

        DasPtr<Das::ExportInterface::IDasJson> result;
        ASSERT_EQ(
            component
                ->Do(nullptr, nullptr, nullptr, input_json.Get(), result.Put()),
            DAS_S_OK);

        auto json = ToJson(result.Get());
        auto obj = json.as_object();
        ASSERT_TRUE(obj.has_value());
        EXPECT_EQ(
            (*obj)[std::string_view("status")].as_string().value(),
            "completed");
        EXPECT_TRUE((*obj)[std::string_view("outputs")].is_object());
        EXPECT_TRUE((*obj)[std::string_view("diagnostics")].is_array());
        auto signals = (*obj)[std::string_view("signals")].as_array();
        ASSERT_TRUE(signals.has_value());
        ASSERT_EQ(signals->size(), 1u);
        EXPECT_EQ((*signals)[0].as_string().value(), expected_signal);
    }
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
