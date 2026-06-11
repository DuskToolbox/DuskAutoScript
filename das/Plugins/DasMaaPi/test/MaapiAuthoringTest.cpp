#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

// Forward declarations for PluginUtils functions (linked from PluginUtils.cpp)
namespace Das::Plugins::DasMaaPi
{
    std::vector<Core::GraphRuntime::Dto::GraphPortDefinitionDto>
    DerivePortDefinitions(const PiCatalog& catalog, const std::string& task_name);

    std::map<std::string, std::string> DerivePortMap(
        const PiCatalog& catalog,
        const std::string& task_name);

    yyjson::value BuildPortDefinitionsJson(
        const std::vector<Core::GraphRuntime::Dto::GraphPortDefinitionDto>&
            ports);
} // namespace Das::Plugins::DasMaaPi

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
                && (*field)[std::string_view("valuePath")].as_string().value_or(
                       "")
                       == value_path)
            {
                return true;
            }
        }
        return false;
    }

    bool OptionArrayContains(
        const yyjson::value& pi,
        std::string_view     array_name,
        std::string_view     option_name)
    {
        auto pi_obj = pi.as_object();
        if (!pi_obj || !pi_obj->contains(array_name))
        {
            return false;
        }
        auto options = (*pi_obj)[array_name].as_array();
        if (!options)
        {
            return false;
        }
        for (auto it = options->begin(); it != options->end(); ++it)
        {
            auto option = it->as_object();
            if (option && option->contains(std::string_view("optionName"))
                && (*option)[std::string_view("optionName")]
                           .as_string()
                           .value_or("")
                       == option_name)
            {
                return true;
            }
        }
        return false;
    }

    bool TaskHasOption(const yyjson::value& task, std::string_view option_name)
    {
        auto task_obj = task.as_object();
        if (!task_obj || !task_obj->contains(std::string_view("options")))
        {
            return false;
        }
        auto options = (*task_obj)[std::string_view("options")].as_array();
        if (!options)
        {
            return false;
        }
        for (auto it = options->begin(); it != options->end(); ++it)
        {
            auto option = it->as_object();
            if (option && option->contains(std::string_view("optionName"))
                && (*option)[std::string_view("optionName")]
                           .as_string()
                           .value_or("")
                       == option_name)
            {
                return true;
            }
        }
        return false;
    }

    std::string TaskOptionInputValue(
        const yyjson::value& task,
        std::string_view     option_name,
        std::string_view     input_name)
    {
        auto task_obj = task.as_object();
        if (!task_obj || !task_obj->contains(std::string_view("options")))
        {
            return {};
        }
        auto options = (*task_obj)[std::string_view("options")].as_array();
        if (!options)
        {
            return {};
        }
        for (auto it = options->begin(); it != options->end(); ++it)
        {
            auto option = it->as_object();
            if (!option || !option->contains(std::string_view("optionName"))
                || (*option)[std::string_view("optionName")]
                           .as_string()
                           .value_or("")
                       != option_name
                || !option->contains(std::string_view("inputValues")))
            {
                continue;
            }
            auto inputs =
                (*option)[std::string_view("inputValues")].as_object();
            if (inputs && inputs->contains(input_name)
                && (*inputs)[input_name].is_string())
            {
                return std::string(
                    (*inputs)[input_name].as_string().value_or(""));
            }
        }
        return {};
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
            ASSERT_EQ(
                plugin_manager_->LoadPlugin(MaapiManifestPath()),
                DAS_S_OK);
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
                    DasIidOf<Das::PluginInterface::
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
    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    auto adapter = (*accepted)[std::string_view("adapter")].as_object();
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(
        (*adapter)[std::string_view("interfacePath")].as_string().value_or(""),
        missing_path);

    auto diagnostics = (*obj)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());
    EXPECT_FALSE(diagnostics->empty());

    auto document = (*obj)[std::string_view("document")];
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "adapter.interfacePath"));
    EXPECT_FALSE(ArrayHasFieldWithValuePath(document, "pi.controllerName"));
}

TEST_F(MaapiAuthoringFixture, ApplyPathSuccessRefreshesDocument)
{
    auto session = CreateSession();
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
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
        (*obj)[std::string_view("sourceFingerprint")].as_string().value_or(""),
        "AuthoringFixture:1.0");

    auto document = (*obj)[std::string_view("document")];
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.controllerName"));
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.resourceName"));
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.options.stage"));
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.options.retry"));
    EXPECT_TRUE(
        ArrayHasFieldWithValuePath(document, "pi.options.use-medicine"));
    EXPECT_TRUE(ArrayHasFieldWithValuePath(document, "pi.options.notify"));
}

TEST_F(MaapiAuthoringFixture, PiValuePathsPersistTasksAndOptions)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   tasks_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"pi.tasks\",\"value\":[{\"taskName\":\"DailyFarm\","
        "\"enabled\":true}]}}");
    ASSERT_EQ(
        session->ApplyChange(tasks_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto stage_change = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"pi.options.stage\",\"value\":{\"selectedCases\":[\"1-1\"]}}}");
    ASSERT_EQ(
        session->ApplyChange(stage_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto retry_change = ParseDasJson(
        "{\"baseRevision\":2,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"pi.options.retry\",\"value\":{\"inputValues\":{\"times\":\"9\"}}}}");
    ASSERT_EQ(
        session->ApplyChange(retry_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto medicine_change = ParseDasJson(
        "{\"baseRevision\":3,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"pi.options.use-medicine\",\"value\":[\"small\",\"large\"]}}");
    ASSERT_EQ(
        session->ApplyChange(medicine_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto notify_change = ParseDasJson(
        "{\"baseRevision\":4,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"pi.options.notify\",\"value\":true}}");
    ASSERT_EQ(
        session->ApplyChange(notify_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto accepted =
        (*result.as_object())[std::string_view("acceptedProperties")]
            .as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")];
    ASSERT_TRUE(OptionArrayContains(pi, "globalOptions", "stage"));

    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    auto tasks = (*pi_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    auto task = (*tasks)[0].as_object();
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(
        (*task)[std::string_view("taskName")].as_string().value_or(""),
        "DailyFarm");

    EXPECT_TRUE(TaskHasOption((*tasks)[0], "retry"));
    EXPECT_EQ(TaskOptionInputValue((*tasks)[0], "retry", "times"), "9");
    EXPECT_TRUE(TaskHasOption((*tasks)[0], "use-medicine"));
    EXPECT_TRUE(TaskHasOption((*tasks)[0], "notify"));

    auto delete_retry = ParseDasJson(
        "{\"baseRevision\":5,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"pi.options.retry\",\"value\":null}}");
    ASSERT_EQ(
        session->ApplyChange(delete_retry.Get(), result_json.Put()),
        DAS_S_OK);
    result = ReadJsonInterface(result_json.Get());
    accepted = (*result.as_object())[std::string_view("acceptedProperties")]
                   .as_object();
    ASSERT_TRUE(accepted.has_value());
    pi = (*accepted)[std::string_view("pi")];
    tasks = (*pi.as_object())[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    EXPECT_FALSE(TaskHasOption((*tasks)[0], "retry"));
}

TEST_F(MaapiAuthoringFixture, PiValuePathCleanupRemovesOrphans)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"orphanPaths\":[\"pi.options:legacy\","
          "\"pi.tasks:MissingTask\"],\"globalOptions\":[{\"optionName\":"
          "\"legacy\",\"kind\":\"select\",\"selectedCases\":[\"old\"]}],"
          "\"tasks\":[{\"taskName\":\"MissingTask\",\"enabled\":true}]}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   delete_option = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"pi.options.legacy\",\"value\":null}}");
    ASSERT_EQ(
        session->ApplyChange(delete_option.Get(), result_json.Put()),
        DAS_S_OK);

    auto replace_tasks = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"pi.tasks\",\"value\":[]}}");
    ASSERT_EQ(
        session->ApplyChange(replace_tasks.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto accepted =
        (*result.as_object())[std::string_view("acceptedProperties")]
            .as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());
    auto global_options = (*pi)[std::string_view("globalOptions")].as_array();
    ASSERT_TRUE(global_options.has_value());
    EXPECT_TRUE(global_options->empty());
    auto tasks = (*pi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    EXPECT_TRUE(tasks->empty());
    auto orphans = (*pi)[std::string_view("orphanPaths")].as_array();
    ASSERT_TRUE(orphans.has_value());
    EXPECT_TRUE(orphans->empty());
}

TEST_F(MaapiAuthoringFixture, PresetAndOrphansProjectIntoSettings)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
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

    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
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
    auto migration = (*document)[std::string_view("migration")].as_object();
    ASSERT_TRUE(migration.has_value());
    auto orphans = (*migration)[std::string_view("orphanPaths")].as_array();
    ASSERT_TRUE(orphans.has_value());
    ASSERT_EQ(orphans->size(), 1u);
    EXPECT_EQ((*orphans)[0].as_string().value_or(""), "pi.tasks:MissingTask");
}

// ---------------------------------------------------------------------------
// Port derivation tests (D-05/D-06/D-07)
// ---------------------------------------------------------------------------

namespace
{
    using PortDefinition = Das::Core::GraphRuntime::Dto::GraphPortDefinitionDto;

    PiOption MakeMockSelectOption(
        const std::string&              name,
        const std::vector<std::string>& case_names)
    {
        PiOption option;
        option.dto.name = name;
        option.dto.type = "select";
        option.dto.label = name + "_label";
        for (const auto& cn : case_names)
        {
            PiCaseDto c;
            c.name = cn;
            option.dto.cases.push_back(std::move(c));
        }
        return option;
    }

    PiOption MakeMockCheckboxOption(const std::string& name)
    {
        PiOption option;
        option.dto.name = name;
        option.dto.type = "checkbox";
        option.dto.label = name + "_label";
        return option;
    }

    PiOption MakeMockInputOption(
        const std::string& name,
        const std::string& pipeline_type)
    {
        PiOption option;
        option.dto.name = name;
        option.dto.type = "input";
        option.dto.label = name + "_label";
        PiInputDto input;
        input.name = name + "_input";
        input.pipeline_type = pipeline_type;
        option.dto.inputs.push_back(std::move(input));
        return option;
    }

    PiOption MakeMockSwitchOption(const std::string& name)
    {
        PiOption option;
        option.dto.name = name;
        option.dto.type = "switch";
        option.dto.label = name + "_label";
        return option;
    }

    PiCatalog MakeMockCatalog(
        const std::string&          task_name,
        const std::vector<PiOption> options)
    {
        PiCatalog catalog;
        PiTask    task;
        task.dto.name = task_name;
        for (const auto& opt : options)
        {
            task.dto.option.push_back(opt.dto.name);
        }
        catalog.tasks.push_back(std::move(task));
        catalog.options = std::move(options);
        return catalog;
    }

    const PortDefinition* FindPort(
        const std::vector<PortDefinition>& ports,
        const std::string&                 port_id)
    {
        for (const auto& p : ports)
        {
            if (p.port_id == port_id)
            {
                return &p;
            }
        }
        return nullptr;
    }
} // namespace

TEST(PortDerivationTest, SelectOptionProducesStringPort)
{
    auto catalog = MakeMockCatalog(
        "TestTask",
        {MakeMockSelectOption("screenshot_mode", {"case_a", "case_b"})});

    auto ports =
        Das::Plugins::DasMaaPi::DerivePortDefinitions(catalog, "TestTask");

    ASSERT_GE(ports.size(), 1u);

    const auto* main_port = FindPort(ports, "screenshot_mode");
    ASSERT_NE(main_port, nullptr);
    EXPECT_EQ(main_port->port_type, "string");

    const auto* child_a = FindPort(ports, "screenshot_mode_case_a");
    ASSERT_NE(child_a, nullptr);
    EXPECT_EQ(child_a->port_type, "string");

    const auto* child_b = FindPort(ports, "screenshot_mode_case_b");
    ASSERT_NE(child_b, nullptr);
    EXPECT_EQ(child_b->port_type, "string");
}

TEST(PortDerivationTest, CheckboxProducesArrayStringPort)
{
    auto catalog =
        MakeMockCatalog("TestTask", {MakeMockCheckboxOption("items")});

    auto ports =
        Das::Plugins::DasMaaPi::DerivePortDefinitions(catalog, "TestTask");

    ASSERT_EQ(ports.size(), 1u);
    EXPECT_EQ(ports[0].port_id, "items");
    EXPECT_EQ(ports[0].port_type, "array<string>");
}

TEST(PortDerivationTest, InputIntProducesIntPort)
{
    auto catalog = MakeMockCatalog(
        "TestTask",
        {MakeMockInputOption("retry_count", "int")});

    auto ports =
        Das::Plugins::DasMaaPi::DerivePortDefinitions(catalog, "TestTask");

    ASSERT_EQ(ports.size(), 1u);
    EXPECT_EQ(ports[0].port_id, "retry_count");
    EXPECT_EQ(ports[0].port_type, "int");
}

TEST(PortDerivationTest, InputStringProducesStringPort)
{
    auto catalog = MakeMockCatalog(
        "TestTask",
        {MakeMockInputOption("output_dir", "string")});

    auto ports =
        Das::Plugins::DasMaaPi::DerivePortDefinitions(catalog, "TestTask");

    ASSERT_EQ(ports.size(), 1u);
    EXPECT_EQ(ports[0].port_id, "output_dir");
    EXPECT_EQ(ports[0].port_type, "string");
}

TEST(PortDerivationTest, SwitchProducesBoolPort)
{
    auto catalog =
        MakeMockCatalog("TestTask", {MakeMockSwitchOption("notify")});

    auto ports =
        Das::Plugins::DasMaaPi::DerivePortDefinitions(catalog, "TestTask");

    ASSERT_EQ(ports.size(), 1u);
    EXPECT_EQ(ports[0].port_id, "notify");
    EXPECT_EQ(ports[0].port_type, "bool");
}

TEST(PortDerivationTest, PortMapCreatesCorrectMapping)
{
    auto catalog = MakeMockCatalog(
        "TestTask",
        {MakeMockSelectOption("screenshot_mode", {"case_a", "case_b"})});

    auto port_map = Das::Plugins::DasMaaPi::DerivePortMap(catalog, "TestTask");

    EXPECT_EQ(port_map.count("screenshot_mode"), 1u);
    EXPECT_EQ(port_map["screenshot_mode"], "screenshot_mode");

    EXPECT_EQ(port_map.count("screenshot_mode_case_a"), 1u);
    EXPECT_EQ(port_map["screenshot_mode_case_a"], "screenshot_mode");

    EXPECT_EQ(port_map.count("screenshot_mode_case_b"), 1u);
    EXPECT_EQ(port_map["screenshot_mode_case_b"], "screenshot_mode");
}

TEST(PortDerivationTest, BuildPortDefinitionsJsonProducesValidArray)
{
    PortDefinition port;
    port.port_id = "test_port";
    port.display_label = "Test Port";
    port.port_type = "string";
    port.is_required = true;
    port.default_value = yyjson::value("default_val");

    auto json = Das::Plugins::DasMaaPi::BuildPortDefinitionsJson({port});
    auto arr = json.as_array();
    ASSERT_TRUE(arr.has_value());
    ASSERT_EQ(arr->size(), 1u);

    auto first = (*arr)[0].as_object();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(
        (*first)[std::string_view("portId")].as_string().value_or(""),
        "test_port");
    EXPECT_EQ(
        (*first)[std::string_view("displayLabel")].as_string().value_or(""),
        "Test Port");
    EXPECT_EQ(
        (*first)[std::string_view("portType")].as_string().value_or(""),
        "string");
    EXPECT_TRUE(
        (*first)[std::string_view("isRequired")].as_bool().value_or(false));
}

TEST_F(MaapiAuthoringFixture, ApplyChangeReturnsDynamicPortsInResultJson)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();

    // Apply interface path
    auto session = CreateSession();
    auto path_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"adapter.interfacePath\",\"value\":\""
        + interface_path + "\"}}");
    DasPtr<Das::ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        session->ApplyChange(path_change.Get(), result_json.Put()),
        DAS_S_OK);

    // Apply a task selection
    auto task_change = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"pi.tasks\",\"value\":[{\"taskName\":\"DailyFarm\","
        "\"enabled\":true}]}}");
    ASSERT_EQ(
        session->ApplyChange(task_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE(obj->contains(std::string_view("dynamic_ports")));

    auto dynamic_ports = (*obj)[std::string_view("dynamic_ports")].as_array();
    ASSERT_TRUE(dynamic_ports.has_value());
    EXPECT_FALSE(dynamic_ports->empty());
}
