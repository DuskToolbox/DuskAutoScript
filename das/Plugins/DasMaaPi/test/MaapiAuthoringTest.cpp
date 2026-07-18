#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/AuthoringProjector.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>

// Forward declarations for PluginUtils functions (linked from PluginUtils.cpp)
namespace Das::Plugins::DasMaaPi
{
    std::vector<Core::GraphRuntime::Dto::GraphPortDefinitionDto>
    DerivePortDefinitions(
        const PiCatalog&   catalog,
        const std::string& task_name);

    std::map<std::string, std::string> DerivePortMap(
        const PiCatalog&   catalog,
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

    bool FormSequenceHasField(
        const yyjson::value& document,
        std::string_view     name)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("formSequence")))
        {
            return false;
        }
        auto fs = (*obj)[std::string_view("formSequence")].as_object();
        if (!fs || !fs->contains(std::string_view("fields")))
        {
            return false;
        }
        auto fields = (*fs)[std::string_view("fields")].as_array();
        if (!fields)
        {
            return false;
        }
        for (auto it = fields->begin(); it != fields->end(); ++it)
        {
            auto field = it->as_object();
            if (field && field->contains(std::string_view("name"))
                && (*field)[std::string_view("name")].as_string().value_or("")
                       == name)
            {
                return true;
            }
        }
        return false;
    }

    void CollectKeysRecursive(
        const yyjson::value&   value,
        std::set<std::string>& keys)
    {
        if (auto obj = value.as_object())
        {
            for (const auto& [key, val] : *obj)
            {
                keys.emplace(std::string(key));
                CollectKeysRecursive(val, keys);
            }
            return;
        }
        if (auto arr = value.as_array())
        {
            for (auto it = arr->begin(); it != arr->end(); ++it)
            {
                CollectKeysRecursive(*it, keys);
            }
        }
    }

    bool ContainsAnyKey(
        const yyjson::value&                    document,
        std::initializer_list<std::string_view> keys)
    {
        std::set<std::string> all;
        CollectKeysRecursive(document, all);
        for (auto key : keys)
        {
            if (all.count(std::string(key)) != 0)
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
            ipc_sp_ =
                Das::Core::IPC::MainProcess::CreateIpcContextShared(false);
            ipc_run_thread_ = std::thread([this]() { ipc_sp_->Run(); });
            plugin_manager_ = std::make_unique<PluginManager>(
                *settings_manager_,
                Das::DasSharedRef<Das::Core::IPC::MainProcess::IIpcContext>(
                    ipc_sp_));
            ASSERT_EQ(plugin_manager_->Initialize(1), DAS_S_OK);
            // 模拟生产 DasHttp 启动设 host path（App.cpp:146），让 loadMode:ipc
            // 的 DasMaaPi.json 能 spawn 真实 DasHost。host
            // 未配置/不存在则跳过。
            std::string das_host_path;
            try
            {
                das_host_path = IpcTestConfig::GetDasHostPath();
            }
            catch (const std::exception& e)
            {
                GTEST_SKIP() << "DasHost path unavailable: " << e.what();
            }
            if (!std::filesystem::exists(das_host_path))
            {
                GTEST_SKIP() << "DasHost not found at: " << das_host_path;
            }
            plugin_manager_->SetHostExePath(das_host_path);
            ASSERT_EQ(
                plugin_manager_->LoadPlugin(MaapiManifestPath()),
                DAS_S_OK);
        }

        void TearDown() override
        {
            if (plugin_manager_)
            {
                plugin_manager_->Shutdown();
            }
            if (ipc_sp_)
            {
                ipc_sp_->RequestStop();
            }
            if (ipc_run_thread_.joinable())
            {
                ipc_run_thread_.join();
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
        std::unique_ptr<PluginManager> plugin_manager_;
        std::shared_ptr<Das::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
        std::thread ipc_run_thread_;
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
    EXPECT_TRUE(FormSequenceHasField(document, "adapter.interfacePath"));
    EXPECT_FALSE(FormSequenceHasField(document, "controllerName"));
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
    EXPECT_TRUE(FormSequenceHasField(document, "controllerName"));
    EXPECT_TRUE(FormSequenceHasField(document, "resourceName"));
    EXPECT_TRUE(FormSequenceHasField(document, "options.stage"));
    EXPECT_FALSE(FormSequenceHasField(document, "options.retry"));
    EXPECT_FALSE(FormSequenceHasField(document, "options.use-medicine"));
    EXPECT_FALSE(FormSequenceHasField(document, "options.notify"));
    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    EXPECT_EQ(
        (*doc_obj)[std::string_view("contractVersion")].as_int().value_or(0),
        1);
    EXPECT_FALSE((*doc_obj)[std::string_view("sourceFingerprint")]
                     .as_string()
                     .value_or("")
                     .empty());
    EXPECT_FALSE(ContainsAnyKey(
        document,
        {"valuePath", "sourceLevel", "piRef", "schema"}));
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
        "\"options.stage\",\"value\":\"1-1\"}}");
    ASSERT_EQ(
        session->ApplyChange(stage_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto retry_change = ParseDasJson(
        "{\"baseRevision\":2,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"options.retry\",\"value\":\"9\"}}");
    ASSERT_EQ(
        session->ApplyChange(retry_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto medicine_change = ParseDasJson(
        "{\"baseRevision\":3,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"options.use-medicine\",\"value\":[\"small\",\"large\"]}}");
    ASSERT_EQ(
        session->ApplyChange(medicine_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto notify_change = ParseDasJson(
        "{\"baseRevision\":4,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"options.notify\",\"value\":true}}");
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
        "\"options.retry\",\"value\":null}}");
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
        + "\"},\"pi\":{\"orphanPaths\":[\"options:legacy\","
          "\"pi.tasks:MissingTask\"],\"globalOptions\":[{\"optionName\":"
          "\"legacy\",\"kind\":\"select\",\"selectedCases\":[\"old\"]}],"
          "\"tasks\":[{\"taskName\":\"MissingTask\",\"enabled\":true}]}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   delete_option = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"options.legacy\",\"value\":null}}");
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

TEST_F(MaapiAuthoringFixture, ContractDocShapeHasFormSequenceAndNoPrivateFields)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"taskName\":\"DailyFarm\","
          "\"enabled\":true}]}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    ASSERT_EQ(session->GetDocument({}, result_json.Put()), DAS_S_OK);
    auto document = ReadJsonInterface(result_json.Get());
    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    EXPECT_EQ(
        (*doc_obj)[std::string_view("contractVersion")].as_int().value_or(0),
        1);
    EXPECT_FALSE((*doc_obj)[std::string_view("sourceFingerprint")]
                     .as_string()
                     .value_or("")
                     .empty());

    auto fs = (*doc_obj)[std::string_view("formSequence")].as_object();
    ASSERT_TRUE(fs.has_value());
    EXPECT_TRUE(fs->contains(std::string_view("fields")));
    auto sequence = (*fs)[std::string_view("sequence")].as_array();
    ASSERT_TRUE(sequence.has_value());
    ASSERT_FALSE(sequence->empty());

    auto first = (*sequence)[0].as_object();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(
        (*first)[std::string_view("id")].as_string().value_or(""),
        "DailyFarm");
    EXPECT_EQ(
        (*first)[std::string_view("componentGuid")].as_string().value_or(""),
        "69F20008-0000-4000-8000-000000000001");
    EXPECT_TRUE(first->contains(std::string_view("fields")));
    EXPECT_TRUE(first->contains(std::string_view("settings")));
    EXPECT_TRUE(first->contains(std::string_view("enabled")));

    EXPECT_FALSE(ContainsAnyKey(
        document,
        {"valuePath", "sourceLevel", "piRef", "schema"}));
}

TEST_F(MaapiAuthoringFixture, ItemLevelSetValueTogglesTaskEnabled)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"taskName\":\"DailyFarm\","
          "\"enabled\":true}]}}");

    auto change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"nodeId\":"
        "\"DailyFarm\",\"field\":\"enabled\",\"value\":false}}");
    DasPtr<Das::ExportInterface::IDasJson> result_json;
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());
    auto accepted =
        (*result.as_object())[std::string_view("acceptedProperties")]
            .as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());
    auto tasks = (*pi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    EXPECT_FALSE((*tasks)[0]
                     .as_object()
                     ->operator[](std::string_view("enabled"))
                     .as_bool()
                     .value_or(true));
}

TEST_F(MaapiAuthoringFixture, RemoveSequenceItemDropsTaskFromPiTasks)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"taskName\":\"DailyFarm\","
          "\"enabled\":true},{\"taskName\":\"Award\",\"enabled\":true}]}}");

    auto change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"removeSequenceItem\",\"payload\":{"
        "\"nodeId\":\"DailyFarm\"}}");
    DasPtr<Das::ExportInterface::IDasJson> result_json;
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());
    auto accepted =
        (*result.as_object())[std::string_view("acceptedProperties")]
            .as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());
    auto tasks = (*pi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    EXPECT_EQ(
        (*tasks)[0]
            .as_object()
            ->operator[](std::string_view("taskName"))
            .as_string()
            .value_or(""),
        "Award");
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

// ---------------------------------------------------------------------------
// I18n $ resolution tests
// ---------------------------------------------------------------------------

namespace
{
    PiCatalog MakeI18nMockCatalog()
    {
        PiCatalog catalog;
        catalog.name = "TestI18n";
        catalog.version = "1.0";

        catalog.translations["zh_cn"] = {
            {"controller.android", "安卓端"},
            {"resource.official", "官服资源"},
            {"task.dailyfarm", "日常任务"},
            {"option.stage", "关卡"},
            {"option.case.1-1", "1-1关"},
            {"option.retry", "重试"},
            {"option.times", "次数"},
            {"option.notify", "通知"}};
        catalog.translations["en"] = {
            {"controller.android", "Android"},
            {"resource.official", "Official Resource"},
            {"task.dailyfarm", "Daily Farm"},
            {"option.stage", "Stage"},
            {"option.case.1-1", "Stage 1-1"},
            {"option.retry", "Retry"},
            {"option.times", "Times"},
            {"option.notify", "Notify"}};

        PiController controller;
        controller.dto.name = "Android";
        controller.dto.type = "Adb";
        controller.dto.label = "$controller.android";
        catalog.controllers.push_back(std::move(controller));

        PiResource resource;
        resource.dto.name = "Official";
        resource.dto.path = {"resource"};
        resource.dto.label = "$resource.official";
        catalog.resources.push_back(std::move(resource));

        PiTask task;
        task.dto.name = "DailyFarm";
        task.dto.entry = "StartDaily";
        task.dto.label = "$task.dailyfarm";
        task.dto.option = {"stage", "retry", "notify"};
        catalog.tasks.push_back(std::move(task));

        PiOption stage_option;
        stage_option.dto.name = "stage";
        stage_option.dto.type = "select";
        stage_option.dto.label = "$option.stage";
        PiCaseDto stage_case;
        stage_case.name = "1-1";
        stage_case.label = "$option.case.1-1";
        stage_option.dto.cases.push_back(std::move(stage_case));
        catalog.options.push_back(std::move(stage_option));

        PiOption retry_option;
        retry_option.dto.name = "retry";
        retry_option.dto.type = "input";
        retry_option.dto.label = "$option.retry";
        PiInputDto retry_input;
        retry_input.name = "times";
        retry_input.label = "$option.times";
        retry_option.dto.inputs.push_back(std::move(retry_input));
        catalog.options.push_back(std::move(retry_option));

        PiOption notify_option;
        notify_option.dto.name = "notify";
        notify_option.dto.type = "switch";
        notify_option.dto.label = "$option.notify";
        catalog.options.push_back(std::move(notify_option));

        // stage 是 global_option（对齐 interface_authoring.jsonc 的语义），
        // 既出现在 catalog.options 也出现在 catalog.global_options —— parser
        // 解析 global_option 时按名复制，两份都保留。
        for (const auto& opt : catalog.options)
        {
            if (opt.dto.name == "stage")
            {
                catalog.global_options.push_back(opt);
                break;
            }
        }

        return catalog;
    }

    std::string GetLabelFromCandidateArray(
        const yyjson::value& arr,
        std::string_view     name)
    {
        auto arr_ptr = arr.as_array();
        if (!arr_ptr)
        {
            return "";
        }
        for (auto it = arr_ptr->begin(); it != arr_ptr->end(); ++it)
        {
            auto obj = it->as_object();
            if (!obj)
            {
                continue;
            }
            auto name_val = (*obj)[std::string_view("name")].as_string();
            if (name_val.value_or("") == name)
            {
                auto label_val = (*obj)[std::string_view("label")].as_string();
                if (label_val)
                {
                    return std::string(*label_val);
                }
            }
        }
        return "";
    }

    std::string GetFieldLabel(
        const yyjson::value& document,
        std::string_view     field_name)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("formSequence")))
        {
            return "";
        }
        auto fs = (*obj)[std::string_view("formSequence")].as_object();
        if (!fs || !fs->contains(std::string_view("fields")))
        {
            return "";
        }
        auto fields = (*fs)[std::string_view("fields")].as_array();
        if (!fields)
        {
            return "";
        }
        for (auto it = fields->begin(); it != fields->end(); ++it)
        {
            auto field = it->as_object();
            if (!field)
            {
                continue;
            }
            auto name_val = (*field)[std::string_view("name")].as_string();
            if (name_val.value_or("") == field_name)
            {
                auto label_val = (*field)[std::string_view("label")].as_string();
                if (label_val)
                {
                    return std::string(*label_val);
                }
            }
        }
        return "";
    }

    std::string GetSequenceItemFieldLabel(
        const yyjson::value& document,
        std::string_view     item_id,
        std::string_view     field_name)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("formSequence")))
        {
            return "";
        }
        auto fs = (*obj)[std::string_view("formSequence")].as_object();
        if (!fs || !fs->contains(std::string_view("sequence")))
        {
            return "";
        }
        auto sequence = (*fs)[std::string_view("sequence")].as_array();
        if (!sequence)
        {
            return "";
        }
        for (auto it = sequence->begin(); it != sequence->end(); ++it)
        {
            auto item = it->as_object();
            if (!item)
            {
                continue;
            }
            auto id_val = (*item)[std::string_view("id")].as_string();
            if (id_val.value_or("") != item_id)
            {
                continue;
            }
            auto item_fields = (*item)[std::string_view("fields")].as_array();
            if (!item_fields)
            {
                return "";
            }
            for (auto fit = item_fields->begin(); fit != item_fields->end();
                 ++fit)
            {
                auto field = fit->as_object();
                if (!field)
                {
                    continue;
                }
                auto name_val =
                    (*field)[std::string_view("name")].as_string();
                if (name_val.value_or("") == field_name)
                {
                    auto label_val =
                        (*field)[std::string_view("label")].as_string();
                    if (label_val)
                    {
                        return std::string(*label_val);
                    }
                }
            }
        }
        return "";
    }
} // namespace

TEST(I18nTest, ResolveI18nResolvesDollarPrefixKeys)
{
    auto catalog = MakeI18nMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    auto document = Das::Plugins::DasMaaPi::ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0);

    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());

    auto catalog_val = (*doc_obj)[std::string_view("catalog")].as_object();
    ASSERT_TRUE(catalog_val.has_value());

    auto controllers_arr =
        (*catalog_val)[std::string_view("controllers")].as_array();
    ASSERT_TRUE(controllers_arr.has_value());
    EXPECT_EQ(
        GetLabelFromCandidateArray(*controllers_arr, "Android"),
        "安卓端");

    auto resources_arr =
        (*catalog_val)[std::string_view("resources")].as_array();
    ASSERT_TRUE(resources_arr.has_value());
    EXPECT_EQ(
        GetLabelFromCandidateArray(*resources_arr, "Official"),
        "官服资源");

    auto tasks_arr = (*catalog_val)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks_arr.has_value());
    EXPECT_EQ(GetLabelFromCandidateArray(*tasks_arr, "DailyFarm"), "日常任务");
}

TEST(I18nTest, ResolveI18nSwitchesLanguageCorrectly)
{
    auto catalog = MakeI18nMockCatalog();

    AcceptedSettingsDto settings_zh;
    settings_zh.pi.language = "zh_cn";

    auto document_zh = ProjectAuthoringDocument(
        settings_zh,
        &catalog,
        {},
        0);

    auto doc_obj_zh = document_zh.as_object();
    ASSERT_TRUE(doc_obj_zh.has_value());
    auto catalog_val_zh =
        (*doc_obj_zh)[std::string_view("catalog")].as_object();
    ASSERT_TRUE(catalog_val_zh.has_value());
    auto controllers_zh =
        (*catalog_val_zh)[std::string_view("controllers")].as_array();
    ASSERT_TRUE(controllers_zh.has_value());
    EXPECT_EQ(
        GetLabelFromCandidateArray(*controllers_zh, "Android"),
        "安卓端");

    AcceptedSettingsDto settings_en;
    settings_en.pi.language = "en";

    auto document_en = ProjectAuthoringDocument(
        settings_en,
        &catalog,
        {},
        0);

    auto doc_obj_en = document_en.as_object();
    ASSERT_TRUE(doc_obj_en.has_value());
    auto catalog_val_en =
        (*doc_obj_en)[std::string_view("catalog")].as_object();
    ASSERT_TRUE(catalog_val_en.has_value());
    auto controllers_en =
        (*catalog_val_en)[std::string_view("controllers")].as_array();
    ASSERT_TRUE(controllers_en.has_value());
    EXPECT_EQ(
        GetLabelFromCandidateArray(*controllers_en, "Android"),
        "Android");
}

TEST(I18nTest, ResolveI18nFallsBackToKeyWhenTranslationNotFound)
{
    auto catalog = MakeI18nMockCatalog();

    catalog.translations["zh_cn"].erase("controller.android");

    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    auto document = Das::Plugins::DasMaaPi::ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0);

    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    auto catalog_val = (*doc_obj)[std::string_view("catalog")].as_object();
    ASSERT_TRUE(catalog_val.has_value());
    auto controllers_arr =
        (*catalog_val)[std::string_view("controllers")].as_array();
    ASSERT_TRUE(controllers_arr.has_value());
    EXPECT_EQ(
        GetLabelFromCandidateArray(*controllers_arr, "Android"),
        "controller.android");
}

TEST(I18nTest, ResolveI18nReturnsOriginalTextWhenNotDollarPrefixed)
{
    auto catalog = MakeI18nMockCatalog();

    PiController raw_controller;
    raw_controller.dto.name = "RawTest";
    raw_controller.dto.type = "Adb";
    raw_controller.dto.label = "RawLabel";
    catalog.controllers.push_back(std::move(raw_controller));

    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    auto document = Das::Plugins::DasMaaPi::ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0);

    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    auto catalog_val = (*doc_obj)[std::string_view("catalog")].as_object();
    ASSERT_TRUE(catalog_val.has_value());
    auto controllers_arr =
        (*catalog_val)[std::string_view("controllers")].as_array();
    ASSERT_TRUE(controllers_arr.has_value());
    EXPECT_EQ(
        GetLabelFromCandidateArray(*controllers_arr, "RawTest"),
        "RawLabel");
}

TEST(I18nTest, LanguageFieldPersistsInAcceptedSettings)
{
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
    settings.pi.language = "en";

    auto serialized =
        Das::Plugins::DasMaaPi::SerializeAcceptedSettings(settings);
    auto obj = serialized.as_object();
    ASSERT_TRUE(obj.has_value());
    auto pi = (*obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());
    auto lang = (*pi)[std::string_view("language")].as_string();
    ASSERT_TRUE(lang.has_value());
    EXPECT_EQ(*lang, "en");
}

TEST(I18nTest, LanguageFieldCanBeSetViaApplySetValueChange)
{
    auto catalog = MakeI18nMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;

    auto change = Das::Utils::ParseYyjsonFromString(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"language\",\"value\":\"en\"}}");
    ASSERT_TRUE(change.has_value());

    Das::Plugins::DasMaaPi::ApplySetValueChange(
        settings, *change, &catalog);

    ASSERT_TRUE(settings.pi.language.has_value());
    EXPECT_EQ(*settings.pi.language, "en");
}

TEST(I18nTest, DefaultLanguageIsZhCnWhenAvailable)
{
    auto catalog = MakeI18nMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;

    auto document = Das::Plugins::DasMaaPi::ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0);

    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    auto fs = (*doc_obj)[std::string_view("formSequence")].as_object();
    ASSERT_TRUE(fs.has_value());
    auto fields = (*fs)[std::string_view("fields")].as_array();
    ASSERT_TRUE(fields.has_value());

    bool found_language_field = false;
    for (auto it = fields->begin(); it != fields->end(); ++it)
    {
        auto field = it->as_object();
        if (!field)
        {
            continue;
        }
        auto name_val = (*field)[std::string_view("name")].as_string();
        if (name_val.value_or("") == "language")
        {
            found_language_field = true;
            auto value_val = (*field)[std::string_view("value")].as_string();
            ASSERT_TRUE(value_val.has_value());
            EXPECT_EQ(*value_val, "zh_cn");
            break;
        }
    }
    EXPECT_TRUE(found_language_field);
}

TEST(I18nTest, OptionFieldLabelsAreResolved)
{
    auto catalog = MakeI18nMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    // sequence[].fields[] 只投影用户已选的 task，故需要把 DailyFarm 选上，
    // 任务级 option（retry/notify）才会出现在 sequence 项里。
    Das::Plugins::DasMaaPi::MaapiPiTaskSettingsDto daily_farm;
    daily_farm.task_name = "DailyFarm";
    daily_farm.enabled = true;
    settings.pi.tasks.push_back(std::move(daily_farm));

    auto document = Das::Plugins::DasMaaPi::ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0);

    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());

    EXPECT_EQ(GetFieldLabel(document, "options.stage"), "关卡");
    // retry/notify 是 task 级 option（在 catalog.options，不在 global_options），
    // DAS-78 后归位到 sequence[].fields[]，不再出现在项目级 fields[]。
    EXPECT_EQ(GetSequenceItemFieldLabel(document, "DailyFarm", "options.retry"), "重试");
    EXPECT_EQ(GetSequenceItemFieldLabel(document, "DailyFarm", "options.notify"), "通知");
}

// ---------------------------------------------------------------------------
// DAS-95 方向 A：option value IR 化测试
// 验证 emit 产出的 value 按 dto.type 翻译成标量（不再泄漏 MAAPI DTO
// {selectedCases, boolValue, inputValues}），并验证 apply 端能 round-trip
// 把标量还原回内部 DTO。
// ---------------------------------------------------------------------------

namespace
{
    // 在 document 的 formSequence.fields[] 里按 name 找 field，返回其 value。
    yyjson::value FindProjectFieldValue(
        const yyjson::value& document,
        std::string_view     field_name)
    {
        auto obj = document.as_object();
        if (!obj)
        {
            return yyjson::value(nullptr);
        }
        auto fs = (*obj)[std::string_view("formSequence")].as_object();
        if (!fs)
        {
            return yyjson::value(nullptr);
        }
        auto fields = (*fs)[std::string_view("fields")].as_array();
        if (!fields)
        {
            return yyjson::value(nullptr);
        }
        for (auto it = fields->begin(); it != fields->end(); ++it)
        {
            auto field = it->as_object();
            if (!field)
            {
                continue;
            }
            if ((*field)[std::string_view("name")].as_string().value_or("")
                == field_name)
            {
                return Das::Utils::CloneYyjsonValue(
                    (*field)[std::string_view("value")]);
            }
        }
        return yyjson::value(nullptr);
    }

    // 在 document 的 formSequence.sequence[item_id].fields[] 里找 field value。
    yyjson::value FindSequenceItemFieldValue(
        const yyjson::value& document,
        std::string_view     item_id,
        std::string_view     field_name)
    {
        auto obj = document.as_object();
        if (!obj)
        {
            return yyjson::value(nullptr);
        }
        auto fs = (*obj)[std::string_view("formSequence")].as_object();
        if (!fs)
        {
            return yyjson::value(nullptr);
        }
        auto sequence = (*fs)[std::string_view("sequence")].as_array();
        if (!sequence)
        {
            return yyjson::value(nullptr);
        }
        for (auto it = sequence->begin(); it != sequence->end(); ++it)
        {
            auto item = it->as_object();
            if (!item)
            {
                continue;
            }
            if ((*item)[std::string_view("id")].as_string().value_or("")
                != item_id)
            {
                continue;
            }
            auto item_fields =
                (*item)[std::string_view("fields")].as_array();
            if (!item_fields)
            {
                return yyjson::value(nullptr);
            }
            for (auto fit = item_fields->begin();
                 fit != item_fields->end();
                 ++fit)
            {
                auto field = fit->as_object();
                if (!field)
                {
                    continue;
                }
                if ((*field)[std::string_view("name")].as_string().value_or("")
                    == field_name)
                {
                    return Das::Utils::CloneYyjsonValue(
                        (*field)[std::string_view("value")]);
                }
            }
        }
        return yyjson::value(nullptr);
    }

    // 构造覆盖所有 option 类型的 catalog：
    //   select / input 单字段 string / input 单字段 int / input 多字段 /
    //   checkbox / switch
    PiCatalog MakeOptionIrMockCatalog()
    {
        using namespace Das::Plugins::DasMaaPi;

        PiCatalog catalog;

        PiTask task;
        task.dto.name    = "T";
        task.dto.entry   = "StartT";
        task.dto.option  = {
            "stage", "retry_text", "retry_count",
            "coord", "tags", "notify"};
        catalog.tasks.push_back(std::move(task));

        auto make_select = [](std::string name, std::string case_name)
        {
            PiOption o;
            o.dto.name = std::move(name);
            o.dto.type = "select";
            PiCaseDto c;
            c.name = std::move(case_name);
            o.dto.cases.push_back(std::move(c));
            return o;
        };

        auto make_input_string = [](std::string name)
        {
            PiOption o;
            o.dto.name = std::move(name);
            o.dto.type = "input";
            PiInputDto i;
            i.name = "value";
            o.dto.inputs.push_back(std::move(i));
            return o;
        };

        auto make_input_int = [](std::string name)
        {
            PiOption o;
            o.dto.name = std::move(name);
            o.dto.type = "input";
            PiInputDto i;
            i.name          = "value";
            i.pipeline_type = "int";
            o.dto.inputs.push_back(std::move(i));
            return o;
        };

        auto make_input_multi = [](std::string name)
        {
            PiOption o;
            o.dto.name = std::move(name);
            o.dto.type = "input";
            PiInputDto a;
            a.name = "x";
            PiInputDto b;
            b.name = "y";
            o.dto.inputs.push_back(std::move(a));
            o.dto.inputs.push_back(std::move(b));
            return o;
        };

        auto make_checkbox = [](std::string name)
        {
            PiOption o;
            o.dto.name = std::move(name);
            o.dto.type = "checkbox";
            PiCaseDto c1;
            c1.name = "small";
            PiCaseDto c2;
            c2.name = "large";
            o.dto.cases.push_back(std::move(c1));
            o.dto.cases.push_back(std::move(c2));
            return o;
        };

        auto make_switch = [](std::string name)
        {
            PiOption o;
            o.dto.name = std::move(name);
            o.dto.type = "switch";
            return o;
        };

        catalog.options.push_back(make_select("stage", "1-1"));
        catalog.options.push_back(make_input_string("retry_text"));
        catalog.options.push_back(make_input_int("retry_count"));
        catalog.options.push_back(make_input_multi("coord"));
        catalog.options.push_back(make_checkbox("tags"));
        catalog.options.push_back(make_switch("notify"));
        return catalog;
    }
} // namespace

TEST(OptionValueIrTest, EmitProducesScalarValuesMatchingValueType)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeOptionIrMockCatalog();

    AcceptedSettingsDto settings;
    MaapiPiTaskSettingsDto task;
    task.task_name = "T";
    task.enabled   = true;

    auto make_option = [](std::string                                name,
                          std::string                                kind,
                          std::vector<std::string>                   cases,
                          std::optional<bool>                        b,
                          std::map<std::string, std::string>         inputs)
    {
        MaapiPiOptionSettingsDto o;
        o.option_name    = std::move(name);
        o.kind           = std::move(kind);
        o.selected_cases = std::move(cases);
        o.bool_value     = b;
        o.input_values   = std::move(inputs);
        return o;
    };

    task.options.push_back(make_option(
        "stage", "select", {"1-1"}, std::nullopt, {}));
    task.options.push_back(make_option(
        "retry_text", "input", {}, std::nullopt, {{"value", "9"}}));
    task.options.push_back(make_option(
        "retry_count", "input", {}, std::nullopt, {{"value", "7"}}));
    task.options.push_back(make_option(
        "coord", "input", {}, std::nullopt, {{"x", "10"}, {"y", "20"}}));
    task.options.push_back(make_option(
        "tags", "checkbox", {"small", "large"}, std::nullopt, {}));
    task.options.push_back(make_option(
        "notify", "switch", {}, true, {}));
    settings.pi.tasks.push_back(std::move(task));

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    // select → string 标量
    auto stage_val = FindSequenceItemFieldValue(document, "T", "options.stage");
    ASSERT_TRUE(stage_val.is_string());
    EXPECT_EQ(stage_val.as_string().value_or(""), "1-1");

    // input 单字段 string → string 标量
    auto retry_text_val =
        FindSequenceItemFieldValue(document, "T", "options.retry_text");
    ASSERT_TRUE(retry_text_val.is_string());
    EXPECT_EQ(retry_text_val.as_string().value_or(""), "9");

    // input 单字段 int → int 标量
    auto retry_count_val =
        FindSequenceItemFieldValue(document, "T", "options.retry_count");
    ASSERT_TRUE(retry_count_val.is_sint());
    EXPECT_EQ(retry_count_val.as_sint().value_or(0), 7);

    // input 多字段 → jsonObject {name: value}
    auto coord_val =
        FindSequenceItemFieldValue(document, "T", "options.coord");
    ASSERT_TRUE(coord_val.is_object());
    {
        auto coord_obj = coord_val.as_object();
        ASSERT_TRUE(coord_obj.has_value());
        ASSERT_TRUE(coord_obj->contains(std::string_view("x")));
        EXPECT_EQ(
            (*coord_obj)[std::string_view("x")].as_string().value_or(""),
            "10");
        ASSERT_TRUE(coord_obj->contains(std::string_view("y")));
        EXPECT_EQ(
            (*coord_obj)[std::string_view("y")].as_string().value_or(""),
            "20");
    }

    // checkbox → jsonArray[string]
    auto tags_val = FindSequenceItemFieldValue(document, "T", "options.tags");
    ASSERT_TRUE(tags_val.is_array());
    {
        auto tags_arr = tags_val.as_array();
        ASSERT_TRUE(tags_arr.has_value());
        ASSERT_EQ(tags_arr->size(), 2u);
        EXPECT_EQ((*tags_arr)[0].as_string().value_or(""), "small");
        EXPECT_EQ((*tags_arr)[1].as_string().value_or(""), "large");
    }

    // switch → bool 标量
    auto notify_val =
        FindSequenceItemFieldValue(document, "T", "options.notify");
    ASSERT_TRUE(notify_val.is_bool());
    EXPECT_TRUE(notify_val.as_bool().value_or(false));
}

TEST(OptionValueIrTest, ApplyRoundTripsScalarValuesBackToDto)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeOptionIrMockCatalog();

    // 通过 ApplySetValueChange 把 IR 标量回灌到 settings，验证 apply 端
    // 能把每种标量形态还原成 acceptedProperties 里的 MaapiPiOptionSettingsDto。
    AcceptedSettingsDto settings;
    MaapiPiTaskSettingsDto task;
    task.task_name = "T";
    task.enabled   = true;
    settings.pi.tasks.push_back(std::move(task));

    auto apply = [&](std::string value_path, std::string value_json_literal)
    {
        const std::string json =
            std::string{"{\"baseRevision\":0,\"kind\":\"setValue\","
                        "\"payload\":{\"valuePath\":\""}
            + value_path + "\",\"value\":" + value_json_literal + "}}";
        auto change = Das::Utils::ParseYyjsonFromString(json);
        ASSERT_TRUE(change.has_value());
        ApplySetValueChange(settings, *change, &catalog);
    };

    apply("options.stage",       "\"1-1\"");                         // select string
    apply("options.retry_text",  "\"hello\"");                       // input 单字段 string
    apply("options.retry_count", "42");                              // input 单字段 int
    apply("options.coord",       "{\"x\":\"100\",\"y\":\"200\"}");   // input 多字段 obj
    apply("options.tags",        "[\"small\"]");                     // checkbox jsonArray
    apply("options.notify",      "true");                            // switch bool

    // 把 acceptedProperties 序列化出来，对照每个 option 的内部 DTO 字段。
    auto serialized = SerializeAcceptedSettings(settings);
    auto s_obj = serialized.as_object();
    ASSERT_TRUE(s_obj.has_value());
    auto pi = (*s_obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());
    auto tasks = (*pi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    auto serialized_task = (*tasks)[0].as_object();
    ASSERT_TRUE(serialized_task.has_value());
    auto options = (*serialized_task)[std::string_view("options")].as_array();
    ASSERT_TRUE(options.has_value());

    auto find_option = [&](std::string_view opt_name)
        -> std::optional<yyjson::value>
    {
        for (auto it = options->begin(); it != options->end(); ++it)
        {
            auto o = it->as_object();
            if (o && (*o)[std::string_view("optionName")].as_string()
                         .value_or("")
                     == opt_name)
            {
                return Das::Utils::CloneYyjsonValue(*it);
            }
        }
        return std::nullopt;
    };

    {
        auto stage = find_option("stage");
        ASSERT_TRUE(stage.has_value());
        auto stage_obj = stage->as_object();
        ASSERT_TRUE(stage_obj.has_value());
        auto arr = (*stage_obj)[std::string_view("selectedCases")].as_array();
        ASSERT_TRUE(arr.has_value());
        ASSERT_EQ(arr->size(), 1u);
        EXPECT_EQ((*arr)[0].as_string().value_or(""), "1-1");
    }
    {
        auto retry_text = find_option("retry_text");
        ASSERT_TRUE(retry_text.has_value());
        auto rt_obj = retry_text->as_object();
        ASSERT_TRUE(rt_obj.has_value());
        ASSERT_TRUE(rt_obj->contains(std::string_view("inputValues")));
        auto iv_obj =
            (*rt_obj)[std::string_view("inputValues")].as_object();
        ASSERT_TRUE(iv_obj.has_value());
        ASSERT_TRUE(iv_obj->contains(std::string_view("value")));
        EXPECT_EQ(
            (*iv_obj)[std::string_view("value")].as_string().value_or(""),
            "hello");
    }
    {
        auto retry_count = find_option("retry_count");
        ASSERT_TRUE(retry_count.has_value());
        auto rc_obj = retry_count->as_object();
        ASSERT_TRUE(rc_obj.has_value());
        ASSERT_TRUE(rc_obj->contains(std::string_view("inputValues")));
        auto iv_obj =
            (*rc_obj)[std::string_view("inputValues")].as_object();
        ASSERT_TRUE(iv_obj.has_value());
        ASSERT_TRUE(iv_obj->contains(std::string_view("value")));
        EXPECT_EQ(
            (*iv_obj)[std::string_view("value")].as_string().value_or(""),
            "42");
    }
    {
        auto coord = find_option("coord");
        ASSERT_TRUE(coord.has_value());
        auto co_obj = coord->as_object();
        ASSERT_TRUE(co_obj.has_value());
        ASSERT_TRUE(co_obj->contains(std::string_view("inputValues")));
        auto iv_obj = (*co_obj)[std::string_view("inputValues")].as_object();
        ASSERT_TRUE(iv_obj.has_value());
        EXPECT_EQ(iv_obj->size(), 2u);
        EXPECT_EQ(
            (*iv_obj)[std::string_view("x")].as_string().value_or(""), "100");
        EXPECT_EQ(
            (*iv_obj)[std::string_view("y")].as_string().value_or(""), "200");
    }
    {
        auto tags = find_option("tags");
        ASSERT_TRUE(tags.has_value());
        auto tg_obj = tags->as_object();
        ASSERT_TRUE(tg_obj.has_value());
        auto arr = (*tg_obj)[std::string_view("selectedCases")].as_array();
        ASSERT_TRUE(arr.has_value());
        ASSERT_EQ(arr->size(), 1u);
        EXPECT_EQ((*arr)[0].as_string().value_or(""), "small");
    }
    {
        auto notify = find_option("notify");
        ASSERT_TRUE(notify.has_value());
        auto nf_obj = notify->as_object();
        ASSERT_TRUE(nf_obj.has_value());
        ASSERT_TRUE(nf_obj->contains(std::string_view("boolValue")));
        EXPECT_TRUE((*nf_obj)[std::string_view("boolValue")]
                        .as_bool()
                        .value_or(false));
    }
}

// ---------------------------------------------------------------------------
// Preset 展开测试（批次 3） - 单元测试（不依赖 IPC）
// ---------------------------------------------------------------------------

namespace
{
    PiCatalog MakePresetMockCatalog()
    {
        PiCatalog catalog;
        catalog.name = "TestPreset";
        catalog.version = "1.0";

        PiTask task;
        task.dto.name = "DailyFarm";
        task.dto.entry = "StartDaily";
        task.dto.default_check = true;
        task.dto.option = {"stage", "retry", "use-medicine", "notify"};
        catalog.tasks.push_back(std::move(task));

        PiOption stage_option;
        stage_option.dto.name = "stage";
        stage_option.dto.type = "select";
        PiCaseDto case_1_1;
        case_1_1.name = "1-1";
        stage_option.dto.cases.push_back(std::move(case_1_1));
        catalog.options.push_back(std::move(stage_option));

        PiOption retry_option;
        retry_option.dto.name = "retry";
        retry_option.dto.type = "input";
        PiInputDto retry_input;
        retry_input.name = "times";
        retry_input.default_value = "3";
        retry_input.pipeline_type = "int";
        retry_option.dto.inputs.push_back(std::move(retry_input));
        catalog.options.push_back(std::move(retry_option));

        PiOption use_medicine_option;
        use_medicine_option.dto.name = "use-medicine";
        use_medicine_option.dto.type = "checkbox";
        PiCaseDto small_case;
        small_case.name = "small";
        PiCaseDto large_case;
        large_case.name = "large";
        use_medicine_option.dto.cases.push_back(std::move(small_case));
        use_medicine_option.dto.cases.push_back(std::move(large_case));
        catalog.options.push_back(std::move(use_medicine_option));

        PiOption notify_option;
        notify_option.dto.name = "notify";
        notify_option.dto.type = "switch";
        catalog.options.push_back(std::move(notify_option));

        PiPreset preset;
        preset.dto.name = "DailyPreset";
        PiPresetTaskDto preset_task;
        preset_task.name = "DailyFarm";
        preset_task.enabled = true;
        preset_task.option_values["stage"] = yyjson::value(std::string("1-1"));
        preset.dto.task.push_back(std::move(preset_task));
        catalog.presets.push_back(std::move(preset));

        return catalog;
    }
}

TEST(PresetTest, PresetNameSetValueTriggersTaskExpansion)
{
    auto catalog = MakePresetMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;

    auto change = Das::Utils::ParseYyjsonFromString(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"presetName\",\"value\":\"DailyPreset\"}}");
    ASSERT_TRUE(change.has_value());

    Das::Plugins::DasMaaPi::ApplySetValueChange(
        settings, *change, &catalog);

    ASSERT_TRUE(settings.pi.preset_name.has_value());
    EXPECT_EQ(*settings.pi.preset_name, "DailyPreset");

    ASSERT_EQ(settings.pi.tasks.size(), 1u);
    EXPECT_EQ(settings.pi.tasks[0].task_name, "DailyFarm");
    EXPECT_TRUE(settings.pi.tasks[0].enabled);

    bool found_stage = false;
    for (const auto& option : settings.pi.tasks[0].options)
    {
        if (option.option_name == "stage")
        {
            found_stage = true;
            ASSERT_EQ(option.selected_cases.size(), 1u);
            EXPECT_EQ(option.selected_cases[0], "1-1");
        }
    }
    EXPECT_TRUE(found_stage);
}

TEST(PresetTest, ApplyPresetExpandsTasksAndRestoresOptionValues)
{
    auto catalog = MakePresetMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;

    Das::Plugins::DasMaaPi::ApplyPreset(settings, catalog, "DailyPreset");

    ASSERT_TRUE(settings.pi.preset_name.has_value());
    EXPECT_EQ(*settings.pi.preset_name, "DailyPreset");

    ASSERT_EQ(settings.pi.tasks.size(), 1u);
    EXPECT_EQ(settings.pi.tasks[0].task_name, "DailyFarm");
    EXPECT_TRUE(settings.pi.tasks[0].enabled);

    bool found_stage = false;
    bool found_retry = false;
    for (const auto& option : settings.pi.tasks[0].options)
    {
        if (option.option_name == "stage")
        {
            found_stage = true;
            ASSERT_EQ(option.selected_cases.size(), 1u);
            EXPECT_EQ(option.selected_cases[0], "1-1");
        }
        if (option.option_name == "retry")
        {
            found_retry = true;
            auto it = option.input_values.find("times");
            ASSERT_TRUE(it != option.input_values.end());
            EXPECT_EQ(it->second, "3");
        }
    }
    EXPECT_TRUE(found_stage);
    EXPECT_TRUE(found_retry);
}

TEST(PresetTest, InvalidPresetNameWritesOrphanPath)
{
    auto catalog = MakePresetMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;

    Das::Plugins::DasMaaPi::ApplyPreset(settings, catalog, "NonExistentPreset");

    EXPECT_FALSE(settings.pi.preset_name.has_value());

    ASSERT_EQ(settings.pi.orphan_paths.size(), 1u);
    EXPECT_EQ(settings.pi.orphan_paths[0], "presetName:NonExistentPreset");
}

TEST(PresetTest, NestedOptionsAreInitializedFromPreset)
{
    auto catalog = MakePresetMockCatalog();

    PiOption select_with_nested;
    select_with_nested.dto.name = "screenshot_mode";
    select_with_nested.dto.type = "select";
    PiCaseDto case_a;
    case_a.name = "case_a";
    case_a.option = {"nested_option"};
    select_with_nested.dto.cases.push_back(std::move(case_a));
    catalog.options.push_back(std::move(select_with_nested));

    PiOption nested_option;
    nested_option.dto.name = "nested_option";
    nested_option.dto.type = "switch";
    catalog.options.push_back(std::move(nested_option));

    PiTask task_with_nested;
    task_with_nested.dto.name = "TaskWithNested";
    task_with_nested.dto.entry = "StartNested";
    task_with_nested.dto.option = {"screenshot_mode"};
    catalog.tasks.push_back(std::move(task_with_nested));

    PiPreset preset_with_nested;
    preset_with_nested.dto.name = "NestedPreset";
    PiPresetTaskDto nested_preset_task;
    nested_preset_task.name = "TaskWithNested";
    nested_preset_task.enabled = true;
    nested_preset_task.option_values["screenshot_mode"] =
        yyjson::value(std::string("case_a"));
    preset_with_nested.dto.task.push_back(std::move(nested_preset_task));
    catalog.presets.push_back(std::move(preset_with_nested));

    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;

    Das::Plugins::DasMaaPi::ApplyPreset(settings, catalog, "NestedPreset");

    ASSERT_EQ(settings.pi.tasks.size(), 1u);
    EXPECT_EQ(settings.pi.tasks[0].task_name, "TaskWithNested");

    bool found_screenshot_mode = false;
    bool found_nested = false;
    for (const auto& option : settings.pi.tasks[0].options)
    {
        if (option.option_name == "screenshot_mode")
        {
            found_screenshot_mode = true;
            ASSERT_EQ(option.selected_cases.size(), 1u);
            EXPECT_EQ(option.selected_cases[0], "case_a");
        }
        if (option.option_name == "nested_option")
        {
            found_nested = true;
            EXPECT_TRUE(option.bool_value.has_value());
            EXPECT_TRUE(*option.bool_value);
        }
    }
    EXPECT_TRUE(found_screenshot_mode);
    EXPECT_TRUE(found_nested);
}

// ---------------------------------------------------------------------------
// Preset 展开测试（批次 3）
// ---------------------------------------------------------------------------

TEST_F(MaapiAuthoringFixture, PresetNameSetValueTriggersTaskExpansion)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto preset_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"presetName\",\"value\":\"DailyPreset\"}}");
    ASSERT_EQ(
        session->ApplyChange(preset_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto accepted =
        (*result.as_object())[std::string_view("acceptedProperties")]
            .as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());

    EXPECT_EQ(
        (*pi)[std::string_view("presetName")].as_string().value_or(""),
        "DailyPreset");

    auto tasks = (*pi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    auto task = (*tasks)[0].as_object();
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(
        (*task)[std::string_view("taskName")].as_string().value_or(""),
        "DailyFarm");
    EXPECT_TRUE(
        (*task)[std::string_view("enabled")].as_bool().value_or(false));

    auto options = (*task)[std::string_view("options")].as_array();
    ASSERT_TRUE(options.has_value());
    bool found_stage = false;
    for (auto it = options->begin(); it != options->end(); ++it)
    {
        auto opt = it->as_object();
        if (!opt)
        {
            continue;
        }
        auto name = (*opt)[std::string_view("optionName")].as_string();
        if (name.value_or("") == "stage")
        {
            found_stage = true;
            auto selected_cases =
                (*opt)[std::string_view("selectedCases")].as_array();
            ASSERT_TRUE(selected_cases.has_value());
            ASSERT_EQ(selected_cases->size(), 1u);
            EXPECT_EQ(
                (*selected_cases)[0].as_string().value_or(""),
                "1-1");
        }
    }
    EXPECT_TRUE(found_stage);
}

TEST_F(MaapiAuthoringFixture, ApplyPresetExpandsTasksAndRestoresOptionValues)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto preset_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"applyPreset\",\"payload\":{"
        "\"presetName\":\"DailyPreset\"}}");
    ASSERT_EQ(
        session->ApplyChange(preset_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto accepted =
        (*result.as_object())[std::string_view("acceptedProperties")]
            .as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());

    auto tasks = (*pi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    auto task = (*tasks)[0].as_object();
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(
        (*task)[std::string_view("taskName")].as_string().value_or(""),
        "DailyFarm");

    auto options = (*task)[std::string_view("options")].as_array();
    ASSERT_TRUE(options.has_value());

    bool found_stage = false;
    bool found_retry = false;
    for (auto it = options->begin(); it != options->end(); ++it)
    {
        auto opt = it->as_object();
        if (!opt)
        {
            continue;
        }
        auto name = (*opt)[std::string_view("optionName")].as_string();
        if (name.value_or("") == "stage")
        {
            found_stage = true;
            auto selected_cases =
                (*opt)[std::string_view("selectedCases")].as_array();
            ASSERT_TRUE(selected_cases.has_value());
            ASSERT_EQ(selected_cases->size(), 1u);
            EXPECT_EQ(
                (*selected_cases)[0].as_string().value_or(""),
                "1-1");
        }
        if (name.value_or("") == "retry")
        {
            found_retry = true;
            auto input_values =
                (*opt)[std::string_view("inputValues")].as_object();
            ASSERT_TRUE(input_values.has_value());
            auto times =
                (*input_values)[std::string_view("times")].as_string();
            ASSERT_TRUE(times.has_value());
            EXPECT_EQ(*times, "3");
        }
    }
    EXPECT_TRUE(found_stage);
    EXPECT_TRUE(found_retry);
}

TEST_F(MaapiAuthoringFixture, InvalidPresetNameWritesOrphanPath)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto preset_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"presetName\",\"value\":\"NonExistentPreset\"}}");
    ASSERT_EQ(
        session->ApplyChange(preset_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto accepted =
        (*result.as_object())[std::string_view("acceptedProperties")]
            .as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());

    EXPECT_EQ(
        (*pi)[std::string_view("presetName")].as_string().value_or(""),
        "NonExistentPreset");

    auto orphan_paths = (*pi)[std::string_view("orphanPaths")].as_array();
    ASSERT_TRUE(orphan_paths.has_value());
    ASSERT_EQ(orphan_paths->size(), 1u);
    EXPECT_EQ(
        (*orphan_paths)[0].as_string().value_or(""),
        "presetName:NonExistentPreset");
}
