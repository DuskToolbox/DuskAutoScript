#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/TaskAuthoringContract.h>
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

    // D-84-13: 根字段直接挂在 document 上（不再位于 formSequence.fields）。
    // 此助手检查根 fields[] 是否包含指定 name。
    bool FormSequenceHasField(
        const yyjson::value& document,
        std::string_view     name)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("fields")))
        {
            return false;
        }
        auto fields = (*obj)[std::string_view("fields")].as_array();
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

    // D-84-13/21/31: 严格 IR 查找助手。在 document 根级 fields[] 按名查找。
    // 提前在首个匿名 namespace 定义，供后续所有 I18nTest / OptionValueIrTest /
    // MaapiProjectionTest 等测试共享。
    std::optional<yyjson::value> FindRootField(
        const yyjson::value& document,
        std::string_view     name)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("fields")))
        {
            return std::nullopt;
        }
        auto fields = (*obj)[std::string_view("fields")].as_array();
        if (!fields)
        {
            return std::nullopt;
        }
        for (auto it = fields->begin(); it != fields->end(); ++it)
        {
            auto field = it->as_object();
            if (!field)
            {
                continue;
            }
            if ((*field)[std::string_view("name")].as_string().value_or("")
                == name)
            {
                return Das::Utils::CloneYyjsonValue(*it);
            }
        }
        return std::nullopt;
    }

    // 从 root.fields[name].candidates[] 中按 value 取候选 label。
    std::string CandidateLabel(
        const yyjson::value& document,
        std::string_view     field_name,
        std::string_view     candidate_value)
    {
        auto field = FindRootField(document, field_name);
        if (!field)
        {
            return "";
        }
        auto field_obj = field->as_object();
        if (!field_obj || !field_obj->contains(std::string_view("candidates")))
        {
            return "";
        }
        auto cands = (*field_obj)[std::string_view("candidates")].as_array();
        if (!cands)
        {
            return "";
        }
        for (auto it = cands->begin(); it != cands->end(); ++it)
        {
            auto cand = it->as_object();
            if (!cand)
            {
                continue;
            }
            if ((*cand)[std::string_view("value")].as_string().value_or("")
                == candidate_value)
            {
                return std::string(
                    (*cand)[std::string_view("label")].as_string().value_or(
                        ""));
            }
        }
        return "";
    }

    // 从 actionCatalog.entries[] 中按 actionId 取候选 label。
    std::string ActionCatalogLabel(
        const yyjson::value& document,
        std::string_view     action_id)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("actionCatalog")))
        {
            return "";
        }
        auto ac = (*obj)[std::string_view("actionCatalog")].as_object();
        if (!ac || !ac->contains(std::string_view("entries")))
        {
            return "";
        }
        auto entries = (*ac)[std::string_view("entries")].as_array();
        if (!entries)
        {
            return "";
        }
        for (auto it = entries->begin(); it != entries->end(); ++it)
        {
            auto entry = it->as_object();
            if (!entry)
            {
                continue;
            }
            if ((*entry)[std::string_view("actionId")].as_string().value_or("")
                == action_id)
            {
                return std::string(
                    (*entry)[std::string_view("label")].as_string().value_or(
                        ""));
            }
        }
        return "";
    }
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
    // D-84-04/11: 严格 Apply envelope = {acceptedProperties, document,
    // diagnostics}；sourceFingerprint / migration 已删除。
    EXPECT_FALSE(obj->contains(std::string_view("sourceFingerprint")));
    EXPECT_FALSE(obj->contains(std::string_view("migration")));
    EXPECT_TRUE(obj->contains(std::string_view("acceptedProperties")));
    EXPECT_TRUE(obj->contains(std::string_view("document")));
    EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));

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
    // D-84-02/04: document 内不再有 kind/sourceFingerprint/catalog；
    // viewKind 是新唯一标识。
    EXPECT_EQ(
        (*doc_obj)[std::string_view("viewKind")].as_string().value_or(""),
        "formSequence");
    EXPECT_FALSE(doc_obj->contains(std::string_view("kind")));
    EXPECT_FALSE(doc_obj->contains(std::string_view("sourceFingerprint")));
    EXPECT_FALSE(doc_obj->contains(std::string_view("catalog")));
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

    // NOTE: yyjson owning values (result/accepted/pi/tasks) cannot be
    // reassigned once their underlying mutable_document is released. Each
    // block scope keeps one contiguous lifecycle so the
    // optional<mutable_object_base> releases its val_ before the owning
    // result's mut_doc is freed. Reassigning them across reads triggers
    // heap-use-after-free in cpp_yyjson set_value (mutable_value_base::
    // operator= reads moved-from val_ after the source doc is released).
    {
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
    }

    auto delete_retry = ParseDasJson(
        "{\"baseRevision\":5,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"options.retry\",\"value\":null}}");
    ASSERT_EQ(
        session->ApplyChange(delete_retry.Get(), result_json.Put()),
        DAS_S_OK);
    {
        auto result = ReadJsonInterface(result_json.Get());
        auto accepted =
            (*result.as_object())[std::string_view("acceptedProperties")]
                .as_object();
        ASSERT_TRUE(accepted.has_value());
        auto pi = (*accepted)[std::string_view("pi")];
        auto tasks = (*pi.as_object())[std::string_view("tasks")].as_array();
        ASSERT_TRUE(tasks.has_value());
        ASSERT_EQ(tasks->size(), 1u);
        EXPECT_FALSE(TaskHasOption((*tasks)[0], "retry"));
    }
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
    // D-84-04: document.migration 已删除；orphanPaths 仅作为 Provider 私有
    // 字段保留在 acceptedProperties.pi 中，不再投影到 document UI IR。
    EXPECT_FALSE(document->contains(std::string_view("migration")));
    auto pi_orphans = (*pi)[std::string_view("orphanPaths")].as_array();
    ASSERT_TRUE(pi_orphans.has_value());
    ASSERT_EQ(pi_orphans->size(), 1u);
    EXPECT_EQ(
        (*pi_orphans)[0].as_string().value_or(""),
        "pi.tasks:MissingTask");
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
    // D-84-13/84-01: document 不再携带 sourceFingerprint/catalog/kind；
    // viewKind 取而代之。
    EXPECT_EQ(
        (*doc_obj)[std::string_view("viewKind")].as_string().value_or(""),
        "formSequence");
    EXPECT_FALSE(doc_obj->contains(std::string_view("sourceFingerprint")));
    EXPECT_FALSE(doc_obj->contains(std::string_view("catalog")));
    EXPECT_FALSE(doc_obj->contains(std::string_view("kind")));

    // D-84-13: 根字段直接挂在 document。
    EXPECT_TRUE(doc_obj->contains(std::string_view("fields")));

    auto fs = (*doc_obj)[std::string_view("formSequence")].as_object();
    ASSERT_TRUE(fs.has_value());
    // formSequence 只承载 items[]。
    EXPECT_FALSE(fs->contains(std::string_view("fields")));
    EXPECT_FALSE(fs->contains(std::string_view("sequence")));
    auto items = (*fs)[std::string_view("items")].as_array();
    ASSERT_TRUE(items.has_value());
    ASSERT_FALSE(items->empty());

    auto first = (*items)[0].as_object();
    ASSERT_TRUE(first.has_value());
    // D-84-40: FormSequenceItem.id = Provider 颁发的 itemId，不再是
    // taskName。DailyFarm 在创建时未显式给 itemId，ParseAcceptedSettings
    // 通过 NormalizeItemIds 分配稳定 item_<n>。
    const auto item_id =
        (*first)[std::string_view("id")].as_string().value_or("");
    EXPECT_FALSE(item_id.empty());
    EXPECT_NE(item_id, "DailyFarm");
    EXPECT_EQ(
        (*first)[std::string_view("componentGuid")].as_string().value_or(""),
        "69F20008-0000-4000-8000-000000000001");
    EXPECT_EQ(
        (*first)[std::string_view("targetKind")].as_string().value_or(""),
        "componentRef");
    EXPECT_TRUE(first->contains(std::string_view("fields")));
    // D-84-07: settings 已删除；可编辑值只在 fields[]。
    EXPECT_FALSE(first->contains(std::string_view("settings")));
    EXPECT_TRUE(first->contains(std::string_view("enabled")));
    // D-84-45: label 不在 item 级（label 走 i18n 入 fields/catalog）。
    EXPECT_FALSE(first->contains(std::string_view("label")));

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

    // D-84-21: CandidateDescriptor 严格 {value, label, icon?}，按 value 查找。
    std::string GetLabelFromCandidateArray(
        const yyjson::value& arr,
        std::string_view     value_key)
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
            auto val = (*obj)[std::string_view("value")].as_string();
            if (val.value_or("") == value_key)
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

    // D-84-13: 根字段位于 document.fields[]，不在 formSequence.fields。
    std::string GetFieldLabel(
        const yyjson::value& document,
        std::string_view     field_name)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("fields")))
        {
            return "";
        }
        auto fields = (*obj)[std::string_view("fields")].as_array();
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
                auto label_val =
                    (*field)[std::string_view("label")].as_string();
                if (label_val)
                {
                    return std::string(*label_val);
                }
            }
        }
        return "";
    }

    // D-84-13: FormSequenceItem 位于 formSequence.items[]（不再叫 sequence）。
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
        if (!fs || !fs->contains(std::string_view("items")))
        {
            return "";
        }
        auto items = (*fs)[std::string_view("items")].as_array();
        if (!items)
        {
            return "";
        }
        for (auto it = items->begin(); it != items->end(); ++it)
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
                auto name_val = (*field)[std::string_view("name")].as_string();
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
    auto                                        catalog = MakeI18nMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    auto document = Das::Plugins::DasMaaPi::ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0);

    // D-84-13/21: controllers/resources 现在是根字段 controllerName/
    // resourceName 的 candidates[{value,label,icon?}]；tasks 是
    // actionCatalog.entries[{actionId,label,...}]。catalog 已删除。
    EXPECT_EQ(CandidateLabel(document, "controllerName", "Android"), "安卓端");
    EXPECT_EQ(CandidateLabel(document, "resourceName", "Official"), "官服资源");
    EXPECT_EQ(ActionCatalogLabel(document, "DailyFarm"), "日常任务");
}

TEST(I18nTest, ResolveI18nSwitchesLanguageCorrectly)
{
    auto catalog = MakeI18nMockCatalog();

    AcceptedSettingsDto settings_zh;
    settings_zh.pi.language = "zh_cn";

    auto document_zh = ProjectAuthoringDocument(settings_zh, &catalog, {}, 0);
    EXPECT_EQ(
        CandidateLabel(document_zh, "controllerName", "Android"),
        "安卓端");

    AcceptedSettingsDto settings_en;
    settings_en.pi.language = "en";

    auto document_en = ProjectAuthoringDocument(settings_en, &catalog, {}, 0);
    EXPECT_EQ(
        CandidateLabel(document_en, "controllerName", "Android"),
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

    // 缺译文时 ProjectAuthoringDocument 退回 $key 形态 (去掉前缀 $)。
    // 这是 PI 协议的 machine key，不应在正常 UI 中出现；D-84-23 要求
    // Provider 用本地化通用文本 + diagnostics 替代，由 84-07 严格化。
    EXPECT_EQ(
        CandidateLabel(document, "controllerName", "Android"),
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

    // D-84-13/21: catalog.controllers 私有字段已删除；候选信息现在挂在根字段
    // controllerName 的 candidates 上。
    EXPECT_EQ(
        CandidateLabel(document, "controllerName", "RawTest"),
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
    auto                                        catalog = MakeI18nMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;

    auto change = Das::Utils::ParseYyjsonFromString(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"language\",\"value\":\"en\"}}");
    ASSERT_TRUE(change.has_value());

    Das::Plugins::DasMaaPi::ApplySetValueChange(settings, *change, &catalog);

    ASSERT_TRUE(settings.pi.language.has_value());
    EXPECT_EQ(*settings.pi.language, "en");
}

TEST(I18nTest, DefaultLanguageIsZhCnWhenAvailable)
{
    auto                                        catalog = MakeI18nMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;

    auto document = Das::Plugins::DasMaaPi::ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0);

    // D-84-13: 根字段直接位于 document.fields[]（不再在 formSequence.fields）。
    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    auto fields = (*doc_obj)[std::string_view("fields")].as_array();
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
    auto                                        catalog = MakeI18nMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    // items[].fields[] 只投影用户已选的 task，故需要把 DailyFarm 选上，
    // 任务级 option（retry/notify）才会出现在 items 项里。
    // D-84-40: item_id 是 Provider 颁发的稳定身份，直接显式给出便于
    // GetSequenceItemFieldLabel 按 id 查找。
    Das::Plugins::DasMaaPi::MaapiPiTaskSettingsDto daily_farm;
    daily_farm.item_id = "item_daily_farm";
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
    // retry/notify 是 task 级 option（在 catalog.options，不在
    // global_options），归位到 items[].fields[]，不再出现在根 fields[]。
    EXPECT_EQ(
        GetSequenceItemFieldLabel(document, "item_daily_farm", "options.retry"),
        "重试");
    EXPECT_EQ(
        GetSequenceItemFieldLabel(
            document,
            "item_daily_farm",
            "options.notify"),
        "通知");
}

// ---------------------------------------------------------------------------
// DAS-95 方向 A：option value IR 化测试
// 验证 emit 产出的 value 按 dto.type 翻译成标量（不再泄漏 MAAPI DTO
// {selectedCases, boolValue, inputValues}），并验证 apply 端能 round-trip
// 把标量还原回内部 DTO。
// ---------------------------------------------------------------------------

namespace
{
    // D-84-13: 根字段位于 document.fields[]（不在 formSequence.fields）。
    yyjson::value FindProjectFieldValue(
        const yyjson::value& document,
        std::string_view     field_name)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("fields")))
        {
            return yyjson::value(nullptr);
        }
        auto fields = (*obj)[std::string_view("fields")].as_array();
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

    // D-84-13: FormSequenceItem 位于 formSequence.items[]（不再叫 sequence）。
    yyjson::value FindSequenceItemFieldValue(
        const yyjson::value& document,
        std::string_view     item_id,
        std::string_view     field_name)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("formSequence")))
        {
            return yyjson::value(nullptr);
        }
        auto fs = (*obj)[std::string_view("formSequence")].as_object();
        if (!fs || !fs->contains(std::string_view("items")))
        {
            return yyjson::value(nullptr);
        }
        auto items = (*fs)[std::string_view("items")].as_array();
        if (!items)
        {
            return yyjson::value(nullptr);
        }
        for (auto it = items->begin(); it != items->end(); ++it)
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
            auto item_fields = (*item)[std::string_view("fields")].as_array();
            if (!item_fields)
            {
                return yyjson::value(nullptr);
            }
            for (auto fit = item_fields->begin(); fit != item_fields->end();
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
        task.dto.name = "T";
        task.dto.entry = "StartT";
        task.dto.option =
            {"stage", "retry_text", "retry_count", "coord", "tags", "notify"};
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
            i.name = "value";
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
            a.pipeline_type = "int";
            PiInputDto b;
            b.name = "y";
            b.pipeline_type = "int";
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

    AcceptedSettingsDto    settings;
    MaapiPiTaskSettingsDto task;
    task.item_id = "T";
    task.task_name = "T";
    task.enabled = true;

    auto make_option = [](std::string                        name,
                          std::string                        kind,
                          std::vector<std::string>           cases,
                          std::optional<bool>                b,
                          std::map<std::string, std::string> inputs)
    {
        MaapiPiOptionSettingsDto o;
        o.option_name = std::move(name);
        o.kind = std::move(kind);
        o.selected_cases = std::move(cases);
        o.bool_value = b;
        o.input_values = std::move(inputs);
        return o;
    };

    task.options.push_back(
        make_option("stage", "select", {"1-1"}, std::nullopt, {}));
    task.options.push_back(
        make_option("retry_text", "input", {}, std::nullopt, {{"value", "9"}}));
    task.options.push_back(make_option(
        "retry_count",
        "input",
        {},
        std::nullopt,
        {{"value", "7"}}));
    task.options.push_back(make_option(
        "coord",
        "input",
        {},
        std::nullopt,
        {{"x", "10"}, {"y", "20"}}));
    task.options.push_back(
        make_option("tags", "checkbox", {"small", "large"}, std::nullopt, {}));
    task.options.push_back(make_option("notify", "switch", {}, true, {}));
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

    // D-84-17: input 多字段不再压成 jsonObject 文本框；每个 input 拆为
    // 独立字段 options.coord.x / options.coord.y，按 input.pipeline_type
    // 决定 valueType（int → 标量 int）。
    auto coord_x_val =
        FindSequenceItemFieldValue(document, "T", "options.coord.x");
    ASSERT_TRUE(coord_x_val.is_sint());
    EXPECT_EQ(coord_x_val.as_sint().value_or(0), 10);
    auto coord_y_val =
        FindSequenceItemFieldValue(document, "T", "options.coord.y");
    ASSERT_TRUE(coord_y_val.is_sint());
    EXPECT_EQ(coord_y_val.as_sint().value_or(0), 20);

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
    AcceptedSettingsDto    settings;
    MaapiPiTaskSettingsDto task;
    task.task_name = "T";
    task.enabled = true;
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

    apply("options.stage", "\"1-1\"");        // select string
    apply("options.retry_text", "\"hello\""); // input 单字段 string
    apply("options.retry_count", "42");       // input 单字段 int
    apply("options.coord", "{\"x\":\"100\",\"y\":\"200\"}"); // input 多字段 obj
    apply("options.tags", "[\"small\"]"); // checkbox jsonArray
    apply("options.notify", "true");      // switch bool

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

    auto find_option =
        [&](std::string_view opt_name) -> std::optional<yyjson::value>
    {
        for (auto it = options->begin(); it != options->end(); ++it)
        {
            auto o = it->as_object();
            if (o
                && (*o)[std::string_view("optionName")].as_string().value_or("")
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
        auto iv_obj = (*rt_obj)[std::string_view("inputValues")].as_object();
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
        auto iv_obj = (*rc_obj)[std::string_view("inputValues")].as_object();
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
            (*iv_obj)[std::string_view("x")].as_string().value_or(""),
            "100");
        EXPECT_EQ(
            (*iv_obj)[std::string_view("y")].as_string().value_or(""),
            "200");
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
        EXPECT_TRUE(
            (*nf_obj)[std::string_view("boolValue")].as_bool().value_or(false));
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

    Das::Plugins::DasMaaPi::ApplySetValueChange(settings, *change, &catalog);

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
    auto                                   preset_change = ParseDasJson(
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
    EXPECT_TRUE((*task)[std::string_view("enabled")].as_bool().value_or(false));

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
            EXPECT_EQ((*selected_cases)[0].as_string().value_or(""), "1-1");
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
    auto                                   preset_change = ParseDasJson(
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
            EXPECT_EQ((*selected_cases)[0].as_string().value_or(""), "1-1");
        }
        if (name.value_or("") == "retry")
        {
            found_retry = true;
            auto input_values =
                (*opt)[std::string_view("inputValues")].as_object();
            ASSERT_TRUE(input_values.has_value());
            auto times = (*input_values)[std::string_view("times")].as_string();
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
    auto                                   preset_change = ParseDasJson(
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

    // Per D-84-04, an invalid preset name must NOT be stored as preset_name
    // (which would imply a successful preset application). The projector only
    // records it as an orphan path on acceptedProperties.pi. The unit-test
    // twin PresetTest.InvalidPresetNameWritesOrphanPath locks the same
    // behavior at the DTO level (preset_name remains unset).
    EXPECT_FALSE((*pi)[std::string_view("presetName")].as_string().has_value());

    auto orphan_paths = (*pi)[std::string_view("orphanPaths")].as_array();
    ASSERT_TRUE(orphan_paths.has_value());
    ASSERT_EQ(orphan_paths->size(), 1u);
    EXPECT_EQ(
        (*orphan_paths)[0].as_string().value_or(""),
        "presetName:NonExistentPreset");
}

// ---------------------------------------------------------------------------
// D-84-05 / D-84-40 / D-84-41 / D-84-42: MaaPI 配置权威 + 稳定 itemId
// 后 4 个测试覆盖：
//   1. 两个相同 taskName 的 task 实例 round-trip 出两个不同的非空 itemId
//   2. newConfiguration 命令创建空配置 + 新身份/名称，不动 PI preset 源
//   3. copyConfiguration 命令创建新配置 + 重新生成的配置/task 身份
//   4. 选择配置改变当前权威；portDefinitions/portMap + pi.orphanPaths 保持私有
// 所有测试经公共 MaaPI authoring 缝 (ParseAcceptedSettings /
// SerializeAcceptedSettings / session Apply) 观察，不测私有 helper。
// ---------------------------------------------------------------------------

namespace
{
    // 在 acceptedProperties.pi.configurations[] 中按 id 查找配置，返回克隆。
    std::optional<yyjson::value> FindConfigurationById(
        const yyjson::value& pi,
        std::string_view     id)
    {
        auto pi_obj = pi.as_object();
        if (!pi_obj || !pi_obj->contains(std::string_view("configurations")))
        {
            return std::nullopt;
        }
        auto arr = (*pi_obj)[std::string_view("configurations")].as_array();
        if (!arr)
        {
            return std::nullopt;
        }
        for (auto it = arr->begin(); it != arr->end(); ++it)
        {
            auto obj = it->as_object();
            if (obj
                && (*obj)[std::string_view("id")].as_string().value_or("")
                       == id)
            {
                return Das::Utils::CloneYyjsonValue(*it);
            }
        }
        return std::nullopt;
    }

    // 读取当前 selected 配置的克隆；selectedConfigurationId 缺失则 nullopt。
    std::optional<yyjson::value> FindSelectedConfiguration(
        const yyjson::value& pi)
    {
        auto pi_obj = pi.as_object();
        if (!pi_obj)
        {
            return std::nullopt;
        }
        auto id =
            (*pi_obj)[std::string_view("selectedConfigurationId")].as_string();
        if (!id.has_value())
        {
            return std::nullopt;
        }
        return FindConfigurationById(pi, *id);
    }

    // 收集一个 configuration 对象内所有 task 的 itemId。
    std::vector<std::string> CollectItemIds(const yyjson::value& config)
    {
        std::vector<std::string> ids;
        auto                     obj = config.as_object();
        if (!obj || !obj->contains(std::string_view("tasks")))
        {
            return ids;
        }
        auto arr = (*obj)[std::string_view("tasks")].as_array();
        if (!arr)
        {
            return ids;
        }
        for (auto it = arr->begin(); it != arr->end(); ++it)
        {
            auto task = it->as_object();
            if (!task)
            {
                continue;
            }
            ids.emplace_back(
                (*task)[std::string_view("itemId")].as_string().value_or(""));
        }
        return ids;
    }
} // namespace

// Behavior 1: 两个相同 taskName 的 task 实例 round-trip 出两个不同的非空
// itemId。
TEST(MaapiConfigurationTest, RoundTripsTwoTasksWithSameNameAndDistinctItemIds)
{
    using namespace Das::Plugins::DasMaaPi;

    auto input = Das::Utils::ParseYyjsonFromString(R"({
        "pi": {
            "selectedConfigurationId": "cfg_alpha",
            "configurations": [{
                "id": "cfg_alpha",
                "name": "Primary",
                "tasks": [
                    {"itemId": "item_a", "taskName": "DailyFarm", "enabled": true},
                    {"itemId": "item_b", "taskName": "DailyFarm", "enabled": false}
                ]
            }]
        }
    })");
    ASSERT_TRUE(input.has_value());

    auto settings = ParseAcceptedSettings(*input);
    auto serialized = SerializeAcceptedSettings(settings);
    auto s_obj = serialized.as_object();
    ASSERT_TRUE(s_obj.has_value());
    auto pi = (*s_obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());

    auto active = FindSelectedConfiguration(*pi);
    ASSERT_TRUE(active.has_value());
    auto tasks = (*active->as_object())[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 2u);

    auto first_id = (*tasks)[0]
                        .as_object()
                        ->operator[](std::string_view("itemId"))
                        .as_string()
                        .value_or("");
    auto second_id = (*tasks)[1]
                         .as_object()
                         ->operator[](std::string_view("itemId"))
                         .as_string()
                         .value_or("");
    EXPECT_FALSE(first_id.empty());
    EXPECT_FALSE(second_id.empty());
    EXPECT_NE(first_id, second_id);

    // 业务类型 taskName 仍可重复， itemId 才是实例身份。
    EXPECT_EQ(
        (*tasks)[0]
            .as_object()
            ->operator[](std::string_view("taskName"))
            .as_string()
            .value_or(""),
        "DailyFarm");
    EXPECT_EQ(
        (*tasks)[1]
            .as_object()
            ->operator[](std::string_view("taskName"))
            .as_string()
            .value_or(""),
        "DailyFarm");
}

// Behavior 2: newConfiguration 创建空配置 + 新身份/名称，不动 PI preset 源。
TEST_F(MaapiAuthoringFixture, NewConfigurationCreatesEmptyAndSelectsIt)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    // 起始状态：一个非空配置 + 一个 PI preset 模板引用，验证 new 不污染
    // preset。
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},"
          "\"pi\":{\"selectedConfigurationId\":\"cfg_seed\",\"configurations\":[{"
          "\"id\":\"cfg_seed\",\"name\":\"Seed\","
          "\"tasks\":[{\"itemId\":\"seed_t1\",\"taskName\":\"DailyFarm\","
          "\"enabled\":true}]}],\"presetName\":\"DailyPreset\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"newConfiguration\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());

    // 新配置必须被选中；其 tasks 必须为空；其 id/name 必须与原配置不同。
    auto active = FindSelectedConfiguration(*pi);
    ASSERT_TRUE(active.has_value());
    auto active_obj = active->as_object();
    ASSERT_TRUE(active_obj.has_value());
    auto active_id =
        (*active_obj)[std::string_view("id")].as_string().value_or("");
    auto active_name =
        (*active_obj)[std::string_view("name")].as_string().value_or("");
    EXPECT_NE(active_id, "cfg_seed");
    EXPECT_FALSE(active_id.empty());
    EXPECT_FALSE(active_name.empty());

    auto active_tasks = (*active_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(active_tasks.has_value());
    EXPECT_TRUE(active_tasks->empty());

    // 原配置仍在，未被破坏。
    auto seed = FindConfigurationById(*pi, "cfg_seed");
    ASSERT_TRUE(seed.has_value());
    auto seed_tasks =
        (*seed->as_object())[std::string_view("tasks")].as_array();
    ASSERT_TRUE(seed_tasks.has_value());
    EXPECT_EQ(seed_tasks->size(), 1u);

    // preset 模板引用保持不变 —— PI 官方 preset 是只读模板。
    EXPECT_EQ(
        (*pi)[std::string_view("presetName")].as_string().value_or(""),
        "DailyPreset");
}

// Behavior 3: copyConfiguration 拷贝当前配置，并重新生成配置+task 身份，
// 保留用户值和顺序。
TEST_F(MaapiAuthoringFixture, CopyConfigurationRegeneratesIdentities)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},"
          "\"pi\":{\"selectedConfigurationId\":\"cfg_orig\",\"configurations\":[{"
          "\"id\":\"cfg_orig\",\"name\":\"Original\","
          "\"controllerName\":\"Android\",\"resourceName\":\"Official\","
          "\"language\":\"zh_cn\","
          "\"tasks\":["
          "{\"itemId\":\"orig_t1\",\"taskName\":\"DailyFarm\",\"enabled\":true},"
          "{\"itemId\":\"orig_t2\",\"taskName\":\"DailyFarm\",\"enabled\":false}"
          "]}]}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"copyConfiguration\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());

    // 必须切换到新配置。
    auto active = FindSelectedConfiguration(*pi);
    ASSERT_TRUE(active.has_value());
    auto active_obj = active->as_object();
    auto active_id =
        (*active_obj)[std::string_view("id")].as_string().value_or("");
    auto active_name =
        (*active_obj)[std::string_view("name")].as_string().value_or("");
    EXPECT_NE(active_id, "cfg_orig");
    EXPECT_FALSE(active_id.empty());
    EXPECT_NE(active_name, "Original");
    EXPECT_FALSE(active_name.empty());

    // 用户值/顺序保留：两个 DailyFarm 仍按原顺序，enabled 状态保留。
    auto active_tasks = (*active_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(active_tasks.has_value());
    ASSERT_EQ(active_tasks->size(), 2u);
    EXPECT_EQ(
        (*active_tasks)[0]
            .as_object()
            ->operator[](std::string_view("taskName"))
            .as_string()
            .value_or(""),
        "DailyFarm");
    EXPECT_EQ(
        (*active_tasks)[1]
            .as_object()
            ->operator[](std::string_view("taskName"))
            .as_string()
            .value_or(""),
        "DailyFarm");
    EXPECT_TRUE((*active_tasks)[0]
                    .as_object()
                    ->operator[](std::string_view("enabled"))
                    .as_bool()
                    .value_or(false));
    EXPECT_FALSE((*active_tasks)[1]
                     .as_object()
                     ->operator[](std::string_view("enabled"))
                     .as_bool()
                     .value_or(true));

    // 身份重新生成：两个新 itemId 都非空，且都不同于原 itemId，且互不相同。
    auto new_ids = CollectItemIds(*active);
    ASSERT_EQ(new_ids.size(), 2u);
    EXPECT_FALSE(new_ids[0].empty());
    EXPECT_FALSE(new_ids[1].empty());
    EXPECT_NE(new_ids[0], new_ids[1]);
    EXPECT_NE(new_ids[0], "orig_t1");
    EXPECT_NE(new_ids[0], "orig_t2");
    EXPECT_NE(new_ids[1], "orig_t1");
    EXPECT_NE(new_ids[1], "orig_t2");

    // 原配置保留原 itemId，未被身份污染。
    auto orig = FindConfigurationById(*pi, "cfg_orig");
    ASSERT_TRUE(orig.has_value());
    auto orig_ids = CollectItemIds(*orig);
    ASSERT_EQ(orig_ids.size(), 2u);
    EXPECT_EQ(orig_ids[0], "orig_t1");
    EXPECT_EQ(orig_ids[1], "orig_t2");

    // 用户值拷贝：controller/resource/language 与原一致。
    EXPECT_EQ(
        (*active_obj)[std::string_view("controllerName")].as_string().value_or(
            ""),
        "Android");
    EXPECT_EQ(
        (*active_obj)[std::string_view("resourceName")].as_string().value_or(
            ""),
        "Official");
    EXPECT_EQ(
        (*active_obj)[std::string_view("language")].as_string().value_or(""),
        "zh_cn");
}

// Behavior 4: 选择配置改变当前权威；portDefinitions/portMap + pi.orphanPaths
// 保持 Provider 私有，不进入共享 UI IR。
TEST_F(MaapiAuthoringFixture, SelectConfigurationSwitchesAuthority)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},"
          "\"pi\":{\"selectedConfigurationId\":\"cfg_a\",\"configurations\":[{"
          "\"id\":\"cfg_a\",\"name\":\"Alpha\","
          "\"tasks\":[{\"itemId\":\"a_t1\",\"taskName\":\"DailyFarm\","
          "\"enabled\":true}]},"
          "{\"id\":\"cfg_b\",\"name\":\"Beta\","
          "\"tasks\":[{\"itemId\":\"b_t1\",\"taskName\":\"DailyFarm\","
          "\"enabled\":true},{\"itemId\":\"b_t2\",\"taskName\":\"DailyFarm\","
          "\"enabled\":false}]}],"
          "\"orphanPaths\":[\"presetName:StalePreset\"]}}");

    // 切换到 cfg_b
    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"configurationId\",\"value\":\"cfg_b\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());

    // 切换后选中 cfg_b。
    EXPECT_EQ(
        (*pi)[std::string_view("selectedConfigurationId")].as_string().value_or(
            ""),
        "cfg_b");
    auto active = FindSelectedConfiguration(*pi);
    ASSERT_TRUE(active.has_value());
    auto active_tasks =
        (*active->as_object())[std::string_view("tasks")].as_array();
    ASSERT_TRUE(active_tasks.has_value());
    EXPECT_EQ(active_tasks->size(), 2u);

    // cfg_a 仍在，未被破坏。
    auto alpha = FindConfigurationById(*pi, "cfg_a");
    ASSERT_TRUE(alpha.has_value());

    // portDefinitions/portMap 仍写入 acceptedProperties（Provider
    // 私有运行期派生）。
    ASSERT_TRUE(accepted->contains(std::string_view("portDefinitions")));
    ASSERT_TRUE(accepted->contains(std::string_view("portMap")));

    // orphanPaths 保留在 pi 层（Provider 私有），不在 document UI IR 中。
    auto orphans = (*pi)[std::string_view("orphanPaths")].as_array();
    ASSERT_TRUE(orphans.has_value());
    ASSERT_EQ(orphans->size(), 1u);
    EXPECT_EQ((*orphans)[0].as_string().value_or(""), "presetName:StalePreset");

    // 文档中不出现私有字段（portDefinitions/portMap/orphanPaths）。
    auto document = (*obj)[std::string_view("document")];
    EXPECT_FALSE(ContainsAnyKey(
        document,
        {"portDefinitions", "portMap", "orphanPaths"}));
}

// ---------------------------------------------------------------------------
// Phase 84-06: 严格 Authoring IR 投影（Get 投影重写）
//
// 这一组测试锁定 D-84-07/13/14/21/23/26/27/31/40/45/46/47 在 MaaPI Get 投影
// 上的最终业务中立 IR 契约：
//   - document 通过 84-01 共享严格 validator
//   - 根字段直接挂在 document，formSequence 只承载 items[]
//   - FormSequenceItem.id = Provider 颁发的 itemId（不接受 taskName 回退）
//   - 不复活 MaaPI 私有字段（catalog/settings/sequence/sourceFingerprint）
//   - valueType/inputType/candidates 严格映射，multi-input 拆为独立字段
//   - validation 从 PI verify/pattern_msg 派生
//   - 所有可见 label 已本地化，缺失翻译产生 diagnostics（不泄漏 $key/英文）
//   - actionCatalog 按 PI task/group 投影；root commands + item debugInfo 到位
//   - icon 仅允许绝对本地路径或 http(s)
//
// 测试经公共 ProjectAuthoringDocument 与 IDasTaskAuthoringSession::GetDocument
// 缝观察，独立手写 literal 期望，绝不引用私有 helper。
// ---------------------------------------------------------------------------

namespace
{
    // 在严格 document 上递归找出第一个 formSequence.items[*] 的 id 值。
    std::string FirstItemId(const yyjson::value& document)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("formSequence")))
        {
            return "";
        }
        auto fs = (*obj)[std::string_view("formSequence")].as_object();
        if (!fs || !fs->contains(std::string_view("items")))
        {
            return "";
        }
        auto items = (*fs)[std::string_view("items")].as_array();
        if (!items || items->empty())
        {
            return "";
        }
        auto first = (*items)[0].as_object();
        if (!first || !first->contains(std::string_view("id")))
        {
            return "";
        }
        return std::string(
            (*first)[std::string_view("id")].as_string().value_or(""));
    }

    // FindRootField / CandidateLabel / ActionCatalogLabel 已在首个匿名
    // namespace 定义（文件顶部 MaapiAuthoringFixture 之后），此处直接复用。

    // 构造一个最小可投影的 catalog：一个
    // controller/resource/task/option/group， 全部带 $key 本地化引用，覆盖
    // zh_cn + en 两种语言。fixture 用于多个 tracer。
    PiCatalog MakeStrictProjectionMockCatalog()
    {
        using namespace Das::Plugins::DasMaaPi;

        PiCatalog catalog;
        catalog.name = "StrictFixture";
        catalog.version = "1.0";
        catalog.translations["zh_cn"] = {
            {"controller.android", "安卓端"},
            {"resource.official", "官服资源"},
            {"task.dailyfarm", "日常任务"},
            {"task.dailyfarm.desc", "日常刷材料"},
            {"group.daily", "日常组"},
            {"option.stage", "关卡"},
            {"option.stage.desc", "选择关卡"},
            {"option.case.1-1", "1-1关"},
            {"option.case.1-2", "1-2关"},
            {"option.coord", "坐标"},
            {"option.coord.x", "X 坐标"},
            {"option.coord.y", "Y 坐标"},
            {"option.notify", "通知"},
            {"option.tags", "标签"},
            {"option.tags.small", "小瓶"},
            {"option.tags.large", "大瓶"},
            {"option.retry", "重试次数"},
            {"option.retry.msg", "请输入正整数"}};
        catalog.translations["en"] = {
            {"controller.android", "Android"},
            {"resource.official", "Official Resource"},
            {"task.dailyfarm", "Daily Farm"},
            {"task.dailyfarm.desc", "Daily material farming"},
            {"group.daily", "Daily Group"},
            {"option.stage", "Stage"},
            {"option.stage.desc", "Choose a stage"},
            {"option.case.1-1", "Stage 1-1"},
            {"option.case.1-2", "Stage 1-2"},
            {"option.coord", "Coordinate"},
            {"option.coord.x", "X"},
            {"option.coord.y", "Y"},
            {"option.notify", "Notify"},
            {"option.tags", "Tags"},
            {"option.tags.small", "Small"},
            {"option.tags.large", "Large"},
            {"option.retry", "Retry Count"},
            {"option.retry.msg", "Must be a positive integer"}};

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

        PiGroup group;
        group.dto.name = "daily";
        group.dto.label = "$group.daily";
        group.dto.default_expand = true;
        catalog.groups.push_back(std::move(group));

        PiTask task;
        task.dto.name = "DailyFarm";
        task.dto.entry = "StartDaily";
        task.dto.label = "$task.dailyfarm";
        task.dto.description = "$task.dailyfarm.desc";
        task.dto.group = {"daily"};
        task.dto.option = {"stage", "coord", "tags", "notify", "retry"};
        catalog.tasks.push_back(std::move(task));

        auto make_select = []() -> PiOption
        {
            PiOption o;
            o.dto.name = "stage";
            o.dto.type = "select";
            o.dto.label = "$option.stage";
            o.dto.description = "$option.stage.desc";
            PiCaseDto c1;
            c1.name = "1-1";
            c1.label = "$option.case.1-1";
            PiCaseDto c2;
            c2.name = "1-2";
            c2.label = "$option.case.1-2";
            o.dto.cases.push_back(std::move(c1));
            o.dto.cases.push_back(std::move(c2));
            return o;
        };

        auto make_multi_input = []() -> PiOption
        {
            PiOption o;
            o.dto.name = "coord";
            o.dto.type = "input";
            o.dto.label = "$option.coord";
            PiInputDto x;
            x.name = "x";
            x.label = "$option.coord.x";
            x.pipeline_type = "int";
            PiInputDto y;
            y.name = "y";
            y.label = "$option.coord.y";
            y.pipeline_type = "int";
            o.dto.inputs.push_back(std::move(x));
            o.dto.inputs.push_back(std::move(y));
            return o;
        };

        auto make_checkbox = []() -> PiOption
        {
            PiOption o;
            o.dto.name = "tags";
            o.dto.type = "checkbox";
            o.dto.label = "$option.tags";
            PiCaseDto small;
            small.name = "small";
            small.label = "$option.tags.small";
            PiCaseDto large;
            large.name = "large";
            large.label = "$option.tags.large";
            o.dto.cases.push_back(std::move(small));
            o.dto.cases.push_back(std::move(large));
            return o;
        };

        auto make_switch = []() -> PiOption
        {
            PiOption o;
            o.dto.name = "notify";
            o.dto.type = "switch";
            o.dto.label = "$option.notify";
            return o;
        };

        auto make_input_int_verified = []() -> PiOption
        {
            PiOption o;
            o.dto.name = "retry";
            o.dto.type = "input";
            o.dto.label = "$option.retry";
            PiInputDto i;
            i.name = "times";
            i.pipeline_type = "int";
            i.verify = "^[1-9][0-9]*$";
            i.pattern_msg = "$option.retry.msg";
            o.dto.inputs.push_back(std::move(i));
            return o;
        };

        catalog.options.push_back(make_select());
        catalog.options.push_back(make_multi_input());
        catalog.options.push_back(make_checkbox());
        catalog.options.push_back(make_switch());
        catalog.options.push_back(make_input_int_verified());
        return catalog;
    }
} // namespace

// Tracer 1: 严格 IR 形态。document 通过 84-01 ValidateAuthoringDocument，
// FormSequenceItem.id = Provider 颁发的 itemId（不接受 taskName 回退），
// document 不复活 catalog/sourceFingerprint/settings/sequence 私有字段。
TEST(MaapiProjectionTest, DocumentPassesStrictContractValidator)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    MaapiPiTaskSettingsDto task;
    task.item_id = "item_strict_1";
    task.task_name = "DailyFarm";
    task.enabled = true;
    settings.pi.tasks.push_back(std::move(task));

    auto document =
        ProjectAuthoringDocument(settings, &catalog, {}, /*revision=*/7);

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(result.valid)
        << "error_kind=" << result.error_kind << " message=" << result.message;

    // 形态不变量：根字段直接挂在 document，formSequence 只承载 items[]。
    auto obj = document.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("viewKind")].as_string().value_or(""),
        "formSequence");
    EXPECT_FALSE(obj->contains(std::string_view("kind")));
    EXPECT_FALSE(obj->contains(std::string_view("catalog")));
    EXPECT_FALSE(obj->contains(std::string_view("sourceFingerprint")));
    EXPECT_FALSE(obj->contains(std::string_view("diagnostics")));
    EXPECT_TRUE(obj->contains(std::string_view("fields")));
    EXPECT_TRUE(obj->contains(std::string_view("actionCatalog")));

    auto fs = (*obj)[std::string_view("formSequence")].as_object();
    ASSERT_TRUE(fs.has_value());
    EXPECT_FALSE(fs->contains(std::string_view("sequence")));
    EXPECT_FALSE(fs->contains(std::string_view("fields")));
    EXPECT_TRUE(fs->contains(std::string_view("items")));

    auto items = (*fs)[std::string_view("items")].as_array();
    ASSERT_TRUE(items.has_value());
    ASSERT_EQ(items->size(), 1u);
    auto item_obj = (*items)[0].as_object();
    ASSERT_TRUE(item_obj.has_value());
    // D-84-40: FormSequenceItem.id = Provider 颁发的 itemId。
    EXPECT_EQ(
        (*item_obj)[std::string_view("id")].as_string().value_or(""),
        "item_strict_1");
    // D-84-07: FormSequenceItem.settings 已删除。
    EXPECT_FALSE(item_obj->contains(std::string_view("settings")));
    // D-84-45: 不把 PI task name / componentGuid 当 label 回退；label 走 i18n。
    EXPECT_FALSE(item_obj->contains(std::string_view("label")));
}

// Tracer 2: multi-input 拆分。D-84-17: PI 多字段 input 投影为一个纵向
// FieldSectionDescriptor 加多个独立 FieldDescriptor，禁止压成 jsonObject
// 文本框。 这里验证 coord (x,y) 被拆为 options.coord.x / options.coord.y
// 两个独立字段， 通过 fieldLayout 的 section 引用，且 apply 端
// setValue{options.coord.x} 能 正确写回 acceptedProperties。
TEST(MaapiProjectionTest, MultiInputOptionSplitsIntoIndependentFields)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    MaapiPiTaskSettingsDto task;
    task.item_id = "item_multi";
    task.task_name = "DailyFarm";
    task.enabled = true;
    MaapiPiOptionSettingsDto coord;
    coord.option_name = "coord";
    coord.kind = "input";
    coord.input_values.emplace("x", "10");
    coord.input_values.emplace("y", "20");
    task.options.push_back(std::move(coord));
    settings.pi.tasks.push_back(std::move(task));

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(result.valid)
        << "error_kind=" << result.error_kind << " message=" << result.message;

    // 取 formSequence.items[0] 的 fields 与 fieldLayout。
    auto obj = document.as_object();
    ASSERT_TRUE(obj.has_value());
    auto fs = (*obj)[std::string_view("formSequence")].as_object();
    ASSERT_TRUE(fs.has_value());
    auto items = (*fs)[std::string_view("items")].as_array();
    ASSERT_TRUE(items.has_value());
    ASSERT_EQ(items->size(), 1u);
    auto item_obj = (*items)[0].as_object();
    ASSERT_TRUE(item_obj.has_value());

    auto item_fields = (*item_obj)[std::string_view("fields")].as_array();
    ASSERT_TRUE(item_fields.has_value());

    // 收集所有 item 字段 name。
    std::set<std::string> item_field_names;
    for (auto it = item_fields->begin(); it != item_fields->end(); ++it)
    {
        auto field = it->as_object();
        ASSERT_TRUE(field.has_value());
        item_field_names.emplace(
            std::string(
                (*field)[std::string_view("name")].as_string().value_or("")));
    }

    // 多字段 input 被拆为独立字段；不再有 jsonObject 形态的 options.coord。
    EXPECT_EQ(item_field_names.count("options.coord"), 0u);
    EXPECT_EQ(item_field_names.count("options.coord.x"), 1u);
    EXPECT_EQ(item_field_names.count("options.coord.y"), 1u);

    // 验证 sub-field 的 label 经 i18n 解析为本地化文本。
    auto find_field = [&](std::string_view name) -> yyjson::value
    {
        for (auto it = item_fields->begin(); it != item_fields->end(); ++it)
        {
            auto field = it->as_object();
            if (field
                && (*field)[std::string_view("name")].as_string().value_or("")
                       == name)
            {
                return Das::Utils::CloneYyjsonValue(*it);
            }
        }
        return yyjson::value(nullptr);
    };
    auto x_field = find_field("options.coord.x");
    ASSERT_TRUE(x_field.is_object());
    EXPECT_EQ(
        x_field.as_object()
            .value()[std::string_view("label")]
            .as_string()
            .value_or(""),
        "X 坐标");
    EXPECT_EQ(
        x_field.as_object()
            .value()[std::string_view("valueType")]
            .as_sint()
            .value_or(-1),
        0); // int
    EXPECT_EQ(
        x_field.as_object()
            .value()[std::string_view("value")]
            .as_sint()
            .value_or(-1),
        10);

    // fieldLayout 必须存在，且包含一个 section 引用 x 与 y。
    ASSERT_TRUE(item_obj->contains(std::string_view("fieldLayout")));
    auto layout = (*item_obj)[std::string_view("fieldLayout")].as_array();
    ASSERT_TRUE(layout.has_value());
    bool found_coord_section = false;
    for (auto it = layout->begin(); it != layout->end(); ++it)
    {
        auto node = it->as_object();
        if (!node || !node->contains(std::string_view("entries")))
        {
            continue;
        }
        auto entries = (*node)[std::string_view("entries")].as_array();
        if (!entries)
        {
            continue;
        }
        std::set<std::string> leaf_names;
        for (auto eit = entries->begin(); eit != entries->end(); ++eit)
        {
            auto leaf = eit->as_object();
            if (!leaf)
            {
                continue;
            }
            leaf_names.emplace(
                std::string(
                    (*leaf)[std::string_view("fieldName")].as_string().value_or(
                        "")));
        }
        if (leaf_names.count("options.coord.x")
            && leaf_names.count("options.coord.y"))
        {
            found_coord_section = true;
            EXPECT_FALSE((*node)[std::string_view("label")]
                             .as_string()
                             .value_or("")
                             .empty());
        }
    }
    EXPECT_TRUE(found_coord_section);
}

// Tracer 2: Apply 端 setValue{options.coord.x} 能写回 acceptedProperties 的
// input_values["x"]。这是 Rule 3 (blocking issue) 自动修复：multi-input 拆分
// 后，UI 编辑 sub-field 时需要新的 apply 路径处理 options.{name}.{input}。
TEST(
    MaapiProjectionTest,
    ApplySetValueOnMultiInputSubfieldWritesBackToInputValues)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();

    AcceptedSettingsDto    settings;
    MaapiPiTaskSettingsDto task;
    task.item_id = "item_apply";
    task.task_name = "DailyFarm";
    MaapiPiOptionSettingsDto coord;
    coord.option_name = "coord";
    coord.kind = "input";
    coord.input_values.emplace("x", "0");
    coord.input_values.emplace("y", "0");
    task.options.push_back(std::move(coord));
    settings.pi.tasks.push_back(std::move(task));

    // setValue{path:"options.coord.x", value:"42"} — 新路径。
    auto change = Das::Utils::ParseYyjsonFromString(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"valuePath\":"
        "\"options.coord.x\",\"value\":\"42\"}}");
    ASSERT_TRUE(change.has_value());
    ApplySetValueChange(settings, *change, &catalog);

    // 通过 SerializeAcceptedSettings 观察结果（不引用私有 helper）。
    auto serialized = SerializeAcceptedSettings(settings);
    auto s_obj = serialized.as_object();
    ASSERT_TRUE(s_obj.has_value());
    auto pi = (*s_obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());
    auto tasks = (*pi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    auto task_obj = (*tasks)[0].as_object();
    ASSERT_TRUE(task_obj.has_value());
    auto options = (*task_obj)[std::string_view("options")].as_array();
    ASSERT_TRUE(options.has_value());

    auto find_option_inputs =
        [&](std::string_view name) -> std::map<std::string, std::string>
    {
        for (auto it = options->begin(); it != options->end(); ++it)
        {
            auto opt = it->as_object();
            if (!opt)
            {
                continue;
            }
            if ((*opt)[std::string_view("optionName")].as_string().value_or("")
                == name)
            {
                std::map<std::string, std::string> result;
                auto                               inputs_obj =
                    (*opt)[std::string_view("inputValues")].as_object();
                if (inputs_obj)
                {
                    for (auto iit = inputs_obj->begin();
                         iit != inputs_obj->end();
                         ++iit)
                    {
                        if (iit->second.is_string())
                        {
                            result.emplace(
                                iit->first,
                                iit->second.as_string().value_or(""));
                        }
                    }
                }
                return result;
            }
        }
        return {};
    };

    auto coord_inputs = find_option_inputs("coord");
    EXPECT_EQ(coord_inputs.count("x"), 1u);
    EXPECT_EQ(coord_inputs.at("x"), "42");
    // y 不变。
    EXPECT_EQ(coord_inputs.count("y"), 1u);
    EXPECT_EQ(coord_inputs.at("y"), "0");
}

// Tracer 3: input verify + pattern_msg → FieldDescriptor.validation{pattern,
// message}。D-84-22/23: PI input.verify regex 映射到 validation.pattern；
// pattern_msg 经当前 language/i18n 解析成最终 message。field 仍由
// MakeInputSubField 产出（retry 是单字段 input，pipeline_type=int）。
TEST(MaapiProjectionTest, InputVerifyAndPatternMsgMapToValidation)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    MaapiPiTaskSettingsDto task;
    task.item_id = "item_verify";
    task.task_name = "DailyFarm";
    MaapiPiOptionSettingsDto retry;
    retry.option_name = "retry";
    retry.kind = "input";
    retry.input_values.emplace("times", "5");
    task.options.push_back(std::move(retry));
    settings.pi.tasks.push_back(std::move(task));

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(result.valid)
        << "error_kind=" << result.error_kind << " message=" << result.message;

    // 在 formSequence.items[0].fields[] 中找 options.retry 字段。
    auto obj = document.as_object();
    ASSERT_TRUE(obj.has_value());
    auto fs = (*obj)[std::string_view("formSequence")].as_object();
    ASSERT_TRUE(fs.has_value());
    auto items = (*fs)[std::string_view("items")].as_array();
    ASSERT_TRUE(items.has_value());
    auto item_obj = (*items)[0].as_object();
    ASSERT_TRUE(item_obj.has_value());
    auto item_fields = (*item_obj)[std::string_view("fields")].as_array();
    ASSERT_TRUE(item_fields.has_value());

    std::optional<yyjson::value> retry_field;
    for (auto it = item_fields->begin(); it != item_fields->end(); ++it)
    {
        auto field = it->as_object();
        if (field
            && (*field)[std::string_view("name")].as_string().value_or("")
                   == "options.retry")
        {
            retry_field = Das::Utils::CloneYyjsonValue(*it);
            break;
        }
    }
    ASSERT_TRUE(retry_field.has_value());
    auto retry_obj = retry_field->as_object();
    ASSERT_TRUE(retry_obj.has_value());
    ASSERT_TRUE(retry_obj->contains(std::string_view("validation")));
    auto validation = (*retry_obj)[std::string_view("validation")].as_object();
    ASSERT_TRUE(validation.has_value());
    // pattern = PI input.verify 原文。
    EXPECT_EQ(
        (*validation)[std::string_view("pattern")].as_string().value_or(""),
        "^[1-9][0-9]*$");
    // message = pattern_msg 经 i18n 解析为当前 language 文本。
    EXPECT_EQ(
        (*validation)[std::string_view("message")].as_string().value_or(""),
        "请输入正整数");
}
