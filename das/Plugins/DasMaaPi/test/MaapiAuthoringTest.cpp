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

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
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

    // D-84-07: 暴露 BuildApplyResult 给 session-level 测试做严格 envelope
    // 校验。
    yyjson::value BuildApplyResult(
        const AcceptedSettingsDto&   settings,
        int64_t                      revision,
        std::vector<PiDiagnosticDto> diagnostics);
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

    // MaapiAuthoringSession::GetDocument returns the strict Phase 84 envelope
    // {document, diagnostics}. Field-level assertions need the nested document
    // payload; the envelope itself is what ValidateAuthoringGetResult expects.
    // ReadJsonInterface remains used by Apply/Compile paths whose root IS the
    // envelope they validate against.
    yyjson::value DocumentOfGet(Das::ExportInterface::IDasJson* get_result)
    {
        auto envelope = ReadJsonInterface(get_result);
        auto obj = envelope.as_object();
        if (!obj || !obj->contains(std::string_view("document")))
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("document")]);
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

    // 从 root.fields[name] 取字段本身的 label（不是 candidate label）。
    // 用于断言根字段 label 是否走 PI i18n 解析。
    std::string RootFieldLabel(
        const yyjson::value& document,
        std::string_view     name)
    {
        auto field = FindRootField(document, name);
        if (!field)
        {
            return "";
        }
        auto field_obj = field->as_object();
        if (!field_obj || !field_obj->contains(std::string_view("label")))
        {
            return "";
        }
        auto label_val =
            (*field_obj)[std::string_view("label")].as_string().value_or("");
        return std::string(label_val);
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
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"adapter.interfacePath\","
        "\"value\":\""
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
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"adapter.interfacePath\","
        "\"value\":\""
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
    // D-84-34/40: 严格 contract 不再支持 pi.tasks 批量替换；初始状态直接通过
    // session context 注入一个 DailyFarm 实例（带显式 itemId 方便后续
    // formSequenceItem-targeted setValue 寻址）。
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_daily_farm\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    // 全局 option stage（target=document，根字段）。
    auto stage_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"options.stage\","
        "\"value\":\"1-1\"}}");
    ASSERT_EQ(
        session->ApplyChange(stage_change.Get(), result_json.Put()),
        DAS_S_OK);

    // task 级 retry（target=formSequenceItem 携带 itemId）。
    auto retry_change = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"formSequenceItem\",\"id\":\"item_daily_farm\"},"
        "\"fieldName\":\"options.retry\",\"value\":\"9\"}}");
    ASSERT_EQ(
        session->ApplyChange(retry_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto medicine_change = ParseDasJson(
        "{\"baseRevision\":2,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"formSequenceItem\",\"id\":\"item_daily_farm\"},"
        "\"fieldName\":\"options.use-medicine\",\"value\":[\"small\","
        "\"large\"]}}");
    ASSERT_EQ(
        session->ApplyChange(medicine_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto notify_change = ParseDasJson(
        "{\"baseRevision\":3,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"formSequenceItem\",\"id\":\"item_daily_farm\"},"
        "\"fieldName\":\"options.notify\",\"value\":true}}");
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
        // stage 是 global option，写入 globalOptions。
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
        "{\"baseRevision\":4,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"formSequenceItem\",\"id\":\"item_daily_farm\"},"
        "\"fieldName\":\"options.retry\",\"value\":null}}");
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
          "\"tasks\":[{\"itemId\":\"item_missing\",\"taskName\":"
          "\"MissingTask\",\"enabled\":true}]}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    // D-84-24: 严格 setValue target=document，fieldName=options.legacy。
    auto delete_option = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"options.legacy\","
        "\"value\":null}}");
    ASSERT_EQ(
        session->ApplyChange(delete_option.Get(), result_json.Put()),
        DAS_S_OK);

    // D-84-35: 严格 removeFormSequenceItem 携带 itemId。
    auto remove_task = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"removeFormSequenceItem\","
        "\"payload\":{\"formSequenceItemId\":\"item_missing\"}}");
    ASSERT_EQ(
        session->ApplyChange(remove_task.Get(), result_json.Put()),
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

    // D-84-37/42: applyPreset 已删除；选择 preset 走 setValue fieldName=
    // presetName，Provider 在 ApplyDocumentSetValue 内部触发 ApplyPreset
    // 展开 task/option 默认值。
    auto preset = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"presetName\","
        "\"value\":\"DailyPreset\"}}");
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
    auto document = DocumentOfGet(result_json.Get());
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
    // item.label/description 是 PI task 本地化展示文案的权威投影；
    // 前端 DasNativeUI 依赖 item.label 显示 task 卡标题，缺失会 fallback
    // 到 id。label 走 PI i18n（task.dto.label），缺译时回退到 task_name。
    EXPECT_TRUE(first->contains(std::string_view("label")));
    EXPECT_FALSE(
        (*first)[std::string_view("label")].as_string().value_or("").empty());
    EXPECT_TRUE(first->contains(std::string_view("description")));

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
        + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_daily_farm\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}");

    // D-84-24: 严格 target=formSequenceItem，按 itemId 寻址。
    auto change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"formSequenceItem\",\"id\":\"item_daily_farm\"},"
        "\"fieldName\":\"enabled\",\"value\":false}}");
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
        + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_daily_farm\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true},{\"itemId\":"
          "\"item_award\",\"taskName\":\"Award\",\"enabled\":true}]}}");

    // D-84-35: 严格 removeFormSequenceItem 按 itemId 寻址。
    auto change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"removeFormSequenceItem\","
        "\"payload\":{\"formSequenceItemId\":\"item_daily_farm\"}}");
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

    // 起始 session 已注入 DailyFarm 实例（带 itemId），避免依赖已删除的
    // pi.tasks 批量替换入口。
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_daily_farm\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}");
    DasPtr<Das::ExportInterface::IDasJson> result_json;
    // D-84-24: 严格 setValue target=document。
    auto path_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"controllerName\","
        "\"value\":\"Android\"}}");
    ASSERT_EQ(
        session->ApplyChange(path_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    // D-84-08/11: 严格 Apply envelope = {acceptedProperties, document,
    // diagnostics}；dynamic_ports 不再出现在 envelope 根（违反 84-01 strict
    // ApplyResult schema）。portDefinitions/portMap 作为 Provider 私有运行期
    // 派生写入 acceptedProperties。
    EXPECT_FALSE(obj->contains(std::string_view("dynamic_ports")));

    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    ASSERT_TRUE(accepted->contains(std::string_view("portDefinitions")));
    auto port_definitions =
        (*accepted)[std::string_view("portDefinitions")].as_array();
    ASSERT_TRUE(port_definitions.has_value());
    EXPECT_FALSE(port_definitions->empty());

    ASSERT_TRUE(accepted->contains(std::string_view("portMap")));
    auto port_map = (*accepted)[std::string_view("portMap")].as_object();
    ASSERT_TRUE(port_map.has_value());
    EXPECT_FALSE(port_map->empty());
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
            {"option.notify", "通知"},
            {"field.controller", "控制器"},
            {"field.resource", "资源"},
            {"field.preset", "预设"},
            {"field.language", "语言"}};
        catalog.translations["en"] = {
            {"controller.android", "Android"},
            {"resource.official", "Official Resource"},
            {"task.dailyfarm", "Daily Farm"},
            {"option.stage", "Stage"},
            {"option.case.1-1", "Stage 1-1"},
            {"option.retry", "Retry"},
            {"option.times", "Times"},
            {"option.notify", "Notify"},
            {"field.controller", "Controller"},
            {"field.resource", "Resource"},
            {"field.preset", "Preset"},
            {"field.language", "Language"}};

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

    // D-84-13: 读 formSequence.items[id].label —— 前端 task 卡标题的权威来源。
    // 缺 key 返回空字符串（调用方用以断言 key 是否被投影）。
    std::string GetSequenceItemLabel(
        const yyjson::value& document,
        std::string_view     item_id)
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
            if ((*item)[std::string_view("id")].as_string().value_or("")
                != item_id)
            {
                continue;
            }
            return std::string(
                (*item)[std::string_view("label")].as_string().value_or(""));
        }
        return "";
    }

    // D-84-13: 读 formSequence.items[id].description。与 GetSequenceItemLabel
    // 不同之处：description 允许空字符串，但 key 仍应存在；用 has_key out
    // 参数让调用方区分 "key 不存在" 与 "key 存在但值为空"。
    std::string GetSequenceItemDescription(
        const yyjson::value& document,
        std::string_view     item_id,
        bool*                has_key)
    {
        if (has_key != nullptr)
        {
            *has_key = false;
        }
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
            if ((*item)[std::string_view("id")].as_string().value_or("")
                != item_id)
            {
                continue;
            }
            if (!item->contains(std::string_view("description")))
            {
                return "";
            }
            if (has_key != nullptr)
            {
                *has_key = true;
            }
            return std::string(
                (*item)[std::string_view("description")].as_string().value_or(
                    ""));
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

// 根字段 label 走 PI i18n 解析：controllerName/resourceName/presetName/language
// 的 label 不能是硬编码协议英文（"Controller"/"Resource"/...），必须按当前
// language 解析 $field.* key。缺译时回退到英文字段名并 emit warning。
TEST(I18nTest, RootFieldLabelsAreLocalizedViaDollarPrefixKeys)
{
    auto catalog = MakeI18nMockCatalog();

    AcceptedSettingsDto settings_zh;
    settings_zh.pi.language = "zh_cn";
    auto document_zh = ProjectAuthoringDocument(settings_zh, &catalog, {}, 0);
    EXPECT_EQ(RootFieldLabel(document_zh, "controllerName"), "控制器");
    EXPECT_EQ(RootFieldLabel(document_zh, "resourceName"), "资源");
    EXPECT_EQ(RootFieldLabel(document_zh, "presetName"), "预设");
    EXPECT_EQ(RootFieldLabel(document_zh, "language"), "语言");

    AcceptedSettingsDto settings_en;
    settings_en.pi.language = "en";
    auto document_en = ProjectAuthoringDocument(settings_en, &catalog, {}, 0);
    EXPECT_EQ(RootFieldLabel(document_en, "controllerName"), "Controller");
    EXPECT_EQ(RootFieldLabel(document_en, "resourceName"), "Resource");
    EXPECT_EQ(RootFieldLabel(document_en, "presetName"), "Preset");
    EXPECT_EQ(RootFieldLabel(document_en, "language"), "Language");
}

// 根字段 label 缺译时回退到 MaaPI 内置中文默认（不依赖 PI i18n，因为这 4 个
// 固定字段的 UI label 在 MaaPI 层面是固定的），并向 projection_diagnostics
// 报告 warning 便于开发者补译。PI i18n 有翻译时优先用 PI 翻译。
TEST(I18nTest, RootFieldLabelFallsBackToChineseDefaultWhenTranslationMissing)
{
    auto catalog = MakeI18nMockCatalog();
    catalog.translations["zh_cn"].erase("field.controller");
    catalog.translations["zh_cn"].erase("field.resource");
    catalog.translations["zh_cn"].erase("field.preset");
    catalog.translations["zh_cn"].erase("field.language");

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    std::vector<PiDiagnosticDto> diags;
    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0, &diags);

    // 4 个固定字段均回退到 MaaPI 内置中文默认，不泄漏英文字段名。
    EXPECT_EQ(RootFieldLabel(document, "controllerName"), "控制器");
    EXPECT_EQ(RootFieldLabel(document, "resourceName"), "资源");
    EXPECT_EQ(RootFieldLabel(document, "presetName"), "预设");
    EXPECT_EQ(RootFieldLabel(document, "language"), "语言");
    // 英文字段名不应出现（PI i18n 缺译时不再 fallback 到英文）。
    EXPECT_NE(RootFieldLabel(document, "controllerName"), "Controller");
    EXPECT_NE(RootFieldLabel(document, "resourceName"), "Resource");
    EXPECT_NE(RootFieldLabel(document, "presetName"), "Preset");
    EXPECT_NE(RootFieldLabel(document, "language"), "Language");

    // 4 个字段都应 emit 缺译 diagnostic。
    int missing_count = 0;
    for (const auto& d : diags)
    {
        if (d.severity == "warning")
        {
            for (const auto* key :
                 {"field.controller",
                  "field.resource",
                  "field.preset",
                  "field.language"})
            {
                if (d.message.find(key) != std::string::npos)
                {
                    ++missing_count;
                    break;
                }
            }
        }
    }
    EXPECT_EQ(missing_count, 4);
}

// 即便 language=en，PI i18n 缺译时也用 MaaPI 内置中文默认（fallback 与
// language 无关，因为内置默认是固定中文 label）。
TEST(I18nTest, RootFieldLabelFallsBackToChineseDefaultEvenForEnglishLanguage)
{
    auto catalog = MakeI18nMockCatalog();
    catalog.translations["en"].erase("field.controller");

    AcceptedSettingsDto settings;
    settings.pi.language = "en";

    std::vector<PiDiagnosticDto> diags;
    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0, &diags);

    EXPECT_EQ(RootFieldLabel(document, "controllerName"), "控制器");
}

// formSequence.items[].label 投影 PI task 的本地化名称。前端 DasNativeUI
// 依赖此 label 显示 task 卡标题；缺失或为空会让前端 fallback 到 id
// （显示 task_1 / item_1）。本测试覆盖：
//  - PI i18n 有译时 label 走译名（zh_cn + en 两种语言）
//  - description key 一定存在（值允许空）
//  - label 非空且不等于 id（不是 fallback）
TEST(I18nTest, FormSequenceItemLabelProjectedFromPiI18n)
{
    auto                                        catalog = MakeI18nMockCatalog();
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    Das::Plugins::DasMaaPi::MaapiPiTaskSettingsDto daily_farm;
    daily_farm.item_id = "item_daily_farm";
    daily_farm.task_name = "DailyFarm";
    daily_farm.enabled = true;
    settings.pi.tasks.push_back(std::move(daily_farm));

    auto document_zh = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    EXPECT_EQ(GetSequenceItemLabel(document_zh, "item_daily_farm"), "日常任务");
    // label 必须非空、且不等于 id（前端依赖 label 区分 task 卡）。
    EXPECT_NE(GetSequenceItemLabel(document_zh, "item_daily_farm"), "");
    EXPECT_NE(
        GetSequenceItemLabel(document_zh, "item_daily_farm"),
        "item_daily_farm");

    // description key 存在；i18n 缺译时允许空字符串（MakeI18nMockCatalog
    // 未给 task.dailyfarm.desc 提供翻译，故 zh_cn 下 description 退到空）。
    bool has_desc = false;
    auto desc =
        GetSequenceItemDescription(document_zh, "item_daily_farm", &has_desc);
    EXPECT_TRUE(has_desc);
    EXPECT_TRUE(desc.empty());

    // 切换到 en：label 走 en 译名；description 仍允许空。
    settings.pi.language = "en";
    auto document_en = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    EXPECT_EQ(
        GetSequenceItemLabel(document_en, "item_daily_farm"),
        "Daily Farm");
    EXPECT_NE(
        GetSequenceItemLabel(document_en, "item_daily_farm"),
        "item_daily_farm");
}

// PI i18n 缺译时，item.label 回退到 task_name（PI 用户可见标识），绝不能
// 回退到 id（item_1 / task_1）。fallback 路径同时向 projection_diagnostics
// emit warning，便于开发者补译。description 缺译时退到空字符串（key 仍在）。
TEST(I18nTest, FormSequenceItemLabelFallsBackToTaskNameWhenTranslationMissing)
{
    auto catalog = MakeI18nMockCatalog();
    catalog.translations["zh_cn"].erase("task.dailyfarm");
    catalog.translations["en"].erase("task.dailyfarm");

    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    Das::Plugins::DasMaaPi::MaapiPiTaskSettingsDto daily_farm;
    daily_farm.item_id = "item_daily_farm";
    daily_farm.task_name = "DailyFarm";
    daily_farm.enabled = true;
    settings.pi.tasks.push_back(std::move(daily_farm));

    std::vector<PiDiagnosticDto> diags;
    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0, &diags);

    const auto label = GetSequenceItemLabel(document, "item_daily_farm");
    // 回退到 task_name（非空、非 id、非 $key、非英文字段名）。
    EXPECT_EQ(label, "DailyFarm");
    EXPECT_NE(label, "item_daily_farm");
    EXPECT_NE(label, "");
    EXPECT_NE(label, "$task.dailyfarm");

    // 缺译应 emit warning，path 指向 formSequence.items.<id>.label。
    bool found_label_diag = false;
    for (const auto& d : diags)
    {
        if (d.severity == "warning"
            && d.message.find("task.dailyfarm") != std::string::npos
            && d.path.has_value()
            && d.path->find("item_daily_farm") != std::string::npos
            && d.path->find(".label") != std::string::npos)
        {
            found_label_diag = true;
            break;
        }
    }
    EXPECT_TRUE(found_label_diag);

    // description key 存在但值为空（fallback 是空字符串）。
    bool has_desc = false;
    auto desc =
        GetSequenceItemDescription(document, "item_daily_farm", &has_desc);
    EXPECT_TRUE(has_desc);
    EXPECT_TRUE(desc.empty());
}

// 多个不同 task_name 的 item，label 各自正确（不相互污染）。
TEST(I18nTest, FormSequenceItemLabelDistinctPerTask)
{
    using namespace Das::Plugins::DasMaaPi;
    PiCatalog catalog;
    catalog.name = "MultiTask";
    catalog.version = "1.0";
    catalog.translations["zh_cn"] = {
        {"task.farm", "日常刷图"},
        {"task.visit", "访问好友"}};

    PiTask farm;
    farm.dto.name = "DailyFarm";
    farm.dto.entry = "StartFarm";
    farm.dto.label = "$task.farm";
    catalog.tasks.push_back(std::move(farm));

    PiTask visit;
    visit.dto.name = "VisitFriends";
    visit.dto.entry = "StartVisit";
    visit.dto.label = "$task.visit";
    catalog.tasks.push_back(std::move(visit));

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    MaapiPiTaskSettingsDto farm_task;
    farm_task.item_id = "item_farm";
    farm_task.task_name = "DailyFarm";
    farm_task.enabled = true;
    settings.pi.tasks.push_back(std::move(farm_task));
    MaapiPiTaskSettingsDto visit_task;
    visit_task.item_id = "item_visit";
    visit_task.task_name = "VisitFriends";
    visit_task.enabled = true;
    settings.pi.tasks.push_back(std::move(visit_task));

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    EXPECT_EQ(GetSequenceItemLabel(document, "item_farm"), "日常刷图");
    EXPECT_EQ(GetSequenceItemLabel(document, "item_visit"), "访问好友");
}

// catalog 缺席（无 PI 接口）时，item.label 退到 task_name（非空），
// description key 仍存在（空字符串）。保证前端离线场景仍能显示稳定标题。
TEST(I18nTest, FormSequenceItemLabelFallsBackWhenCatalogAbsent)
{
    Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    Das::Plugins::DasMaaPi::MaapiPiTaskSettingsDto daily_farm;
    daily_farm.item_id = "item_daily_farm";
    daily_farm.task_name = "DailyFarm";
    daily_farm.enabled = true;
    settings.pi.tasks.push_back(std::move(daily_farm));

    auto document = ProjectAuthoringDocument(settings, nullptr, {}, 0);
    EXPECT_EQ(GetSequenceItemLabel(document, "item_daily_farm"), "DailyFarm");
    EXPECT_NE(GetSequenceItemLabel(document, "item_daily_farm"), "");
    EXPECT_NE(
        GetSequenceItemLabel(document, "item_daily_farm"),
        "item_daily_farm");

    bool has_desc = false;
    /*auto desc =*/GetSequenceItemDescription(
        document,
        "item_daily_farm",
        &has_desc);
    EXPECT_TRUE(has_desc);
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

    std::vector<Das::Plugins::DasMaaPi::PiDiagnosticDto> diags;
    auto document = Das::Plugins::DasMaaPi::ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0,
        &diags);

    // D-84-23: 缺译时不再泄漏 $key/protocol English；Provider 回退到 PI
    // 用户可见标识（dto.name）并向 projection_diagnostics 报告 warning。
    const auto label = CandidateLabel(document, "controllerName", "Android");
    EXPECT_FALSE(label.empty());
    EXPECT_NE(label, "controller.android");
    EXPECT_NE(label, "$controller.android");
    // dto.name 作为最终 fallback，是 PI 协议层面的用户可见标识。
    EXPECT_EQ(label, "Android");
    ASSERT_FALSE(diags.empty());
    bool found_missing = false;
    for (const auto& d : diags)
    {
        if (d.severity == "warning"
            && d.message.find("controller.android") != std::string::npos)
        {
            found_missing = true;
            break;
        }
    }
    EXPECT_TRUE(found_missing);
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
    // D-84-37/42: applyPreset 已删除；preset 选择通过 setValue target=document
    // fieldName=presetName，Provider 在 ApplyDocumentSetValue 中调用
    // ApplyPreset 展开 task/option 默认值。
    auto preset_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"presetName\","
        "\"value\":\"DailyPreset\"}}");
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
    // D-84-37/42: applyPreset 已删除；preset 选择通过 setValue target=document
    // fieldName=presetName 实现。Provider 在 ApplyDocumentSetValue 中触发
    // ApplyPreset 展开 DailyFarm + 默认 option 值。
    auto preset_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"presetName\","
        "\"value\":\"DailyPreset\"}}");
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
    // D-84-24: 严格 setValue target=document fieldName=presetName。未知
    // preset name 触发 Provider 语义失败：ApplyDocumentSetValue emit error
    // 诊断并返回 false，session 不递增 revision，preset_name 保持 unset。
    // orphan path 在 PiCompiler 内部仍可能写入，但严格 Apply envelope 只
    // 关心 diagnostics + 未变更的 acceptedProperties。
    auto preset_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"presetName\","
        "\"value\":\"NonExistentPreset\"}}");
    ASSERT_EQ(
        session->ApplyChange(preset_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    auto pi = (*accepted)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());

    // Per D-84-04/10: 未知 preset 是语义失败。preset_name 不被写入；
    // document.revision 保持 baseRevision（无递增）；envelope 携带 error
    // severity 诊断。
    EXPECT_FALSE((*pi)[std::string_view("presetName")].as_string().has_value());
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("revision")].as_int().value_or(-1),
        0);
    auto diagnostics = (*obj)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());
    bool has_error = false;
    for (auto it = diagnostics->begin(); it != diagnostics->end(); ++it)
    {
        auto diag = it->as_object();
        if (diag
            && (*diag)[std::string_view("severity")].as_string().value_or("")
                   == "error")
        {
            has_error = true;
            break;
        }
    }
    EXPECT_TRUE(has_error);
}

// ---------------------------------------------------------------------------
// D-84-40 / D-84.2: MaaPI flat pi 稳定 itemId round-trip。
// configurations[] 多配置变体模型已删除 (D-84.2-01/02)；本测试覆盖 flat
// pi.tasks[] 上两个相同 taskName 的 task 实例 round-trip 出两个不同的非空
// itemId（D-84-40 不变量）。
// ---------------------------------------------------------------------------

// 两个相同 taskName 的 task 实例 round-trip 出两个不同的非空 itemId。
TEST(MaapiConfigurationTest, RoundTripsTwoTasksWithSameNameAndDistinctItemIds)
{
    using namespace Das::Plugins::DasMaaPi;

    // flat pi seed：无 configurations，直接 pi.tasks[] 两个 DailyFarm item。
    auto input = Das::Utils::ParseYyjsonFromString(R"({
        "pi": {
            "tasks": [
                {"itemId": "item_a", "taskName": "DailyFarm", "enabled": true},
                {"itemId": "item_b", "taskName": "DailyFarm", "enabled": false}
            ]
        }
    })");
    ASSERT_TRUE(input.has_value());

    auto settings = ParseAcceptedSettings(*input);
    auto serialized = SerializeAcceptedSettings(settings);
    auto s_obj = serialized.as_object();
    ASSERT_TRUE(s_obj.has_value());
    auto pi = (*s_obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi.has_value());

    auto tasks = (*pi)[std::string_view("tasks")].as_array();
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
    // item.label 现在是 PI task 本地化标题的权威投影（走 i18n，缺译时
    // 回退到 task_name，绝不回退到 id/componentGuid）。
    EXPECT_TRUE(item_obj->contains(std::string_view("label")));
    EXPECT_FALSE((*item_obj)[std::string_view("label")]
                     .as_string()
                     .value_or("")
                     .empty());
    EXPECT_NE(
        (*item_obj)[std::string_view("label")].as_string().value_or(""),
        "item_strict_1");
    EXPECT_TRUE(item_obj->contains(std::string_view("description")));
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

// Tracer 4: 缺译文不泄漏 $key/machine name；envelope 报告 diagnostic。
// D-84-23: Provider 必须用本地化通用文本替代缺译标签，并通过 envelope
// diagnostics 报告。$key/machine name 不能直接出现在 UI。
TEST(MaapiProjectionTest, MissingTranslationYieldsGenericFallbackAndDiagnostic)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    // 删除 zh_cn 下 controller.android 的翻译，模拟缺译。
    catalog.translations["zh_cn"].erase("controller.android");

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    std::vector<PiDiagnosticDto> projection_diagnostics;
    auto                         document = ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0,
        &projection_diagnostics);

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(result.valid)
        << "error_kind=" << result.error_kind << " message=" << result.message;

    // Controller label 必须不是 $key / machine name。$key 形态是
    // "controller.android"（去掉 $ 前缀），缺译回退不能等于它。
    const auto label = CandidateLabel(document, "controllerName", "Android");
    EXPECT_FALSE(label.empty());
    EXPECT_NE(label, "controller.android");
    EXPECT_NE(label, "$controller.android");

    // 必须有至少一条缺译 diagnostic，severity=warning，message 非空。
    ASSERT_FALSE(projection_diagnostics.empty());
    bool found_missing = false;
    for (const auto& diag : projection_diagnostics)
    {
        if (diag.severity == "warning" && !diag.message.empty())
        {
            found_missing = true;
            // message 提及缺失的 key 与 language，方便开发者定位。
            EXPECT_NE(
                diag.message.find("controller.android"),
                std::string::npos);
            break;
        }
    }
    EXPECT_TRUE(found_missing);
}

// Tracer 5: 根级 commands + item debugInfo。D-84-26/27/28/46.
TEST(MaapiProjectionTest, RootCommandsAndItemDebugInfoProjected)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    MaapiPiTaskSettingsDto task;
    task.item_id = "item_debug";
    task.task_name = "DailyFarm";
    task.enabled = true;
    settings.pi.tasks.push_back(std::move(task));

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(result.valid)
        << "error_kind=" << result.error_kind << " message=" << result.message;

    auto obj = document.as_object();
    ASSERT_TRUE(obj.has_value());

    ASSERT_TRUE(obj->contains(std::string_view("commands")));
    auto commands = (*obj)[std::string_view("commands")].as_array();
    ASSERT_TRUE(commands.has_value());
    std::set<std::string> command_ids;
    for (auto it = commands->begin(); it != commands->end(); ++it)
    {
        auto cmd = it->as_object();
        ASSERT_TRUE(cmd.has_value());
        auto id_opt = (*cmd)[std::string_view("id")].as_string().value_or("");
        auto label_opt =
            (*cmd)[std::string_view("label")].as_string().value_or("");
        EXPECT_FALSE(id_opt.empty());
        EXPECT_FALSE(label_opt.empty());
        command_ids.emplace(std::string(id_opt));
        ASSERT_TRUE(cmd->contains(std::string_view("apply")));
        auto apply = (*cmd)[std::string_view("apply")].as_object();
        ASSERT_TRUE(apply.has_value());
        EXPECT_FALSE(apply->contains(std::string_view("baseRevision")));
        EXPECT_EQ(
            (*apply)[std::string_view("kind")].as_string().value_or(""),
            "invokeCommand");
    }
    // D-84.2-04: newPreset 是无条件 emit 的唯一 root command。
    EXPECT_EQ(command_ids.count("newPreset"), 1u);

    auto fs = (*obj)[std::string_view("formSequence")].as_object();
    ASSERT_TRUE(fs.has_value());
    auto items = (*fs)[std::string_view("items")].as_array();
    ASSERT_TRUE(items.has_value());
    ASSERT_EQ(items->size(), 1u);
    auto item_obj = (*items)[0].as_object();
    ASSERT_TRUE(item_obj.has_value());
    ASSERT_TRUE(item_obj->contains(std::string_view("debugInfo")));
    auto debug_info = (*item_obj)[std::string_view("debugInfo")].as_object();
    ASSERT_TRUE(debug_info.has_value());
    EXPECT_EQ(
        (*debug_info)[std::string_view("piTaskName")].as_string().value_or(""),
        "DailyFarm");
    EXPECT_EQ(
        (*debug_info)[std::string_view("piEntry")].as_string().value_or(""),
        "StartDaily");
    // item.label/description 现在是 PI task 本地化展示文案的权威投影；
    // taskName/entry 不在 label/可见 UI（D-84-45 保留：debugInfo 是 opaque
    // 调试元数据，与 label 的展示角色互不重叠）。
    EXPECT_TRUE(item_obj->contains(std::string_view("label")));
    EXPECT_FALSE(item_obj->operator[](std::string_view("label"))
                     .as_string()
                     .value_or("")
                     .empty());
    EXPECT_TRUE(item_obj->contains(std::string_view("description")));
}

// Tracer 6: icon 解析为绝对本地路径或 http(s)。
TEST(MaapiProjectionTest, IconsResolvedToAbsoluteOrHttps)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    catalog.interface_directory = "C:/maapi/strict";
    catalog.controllers.front().dto.icon = "https://example.com/android.png";
    catalog.tasks.front().dto.icon = "C:/maapi/icons/daily.png";
    for (auto& opt : catalog.options)
    {
        if (opt.dto.name == "stage")
        {
            opt.dto.icon = "icons/stage.png";
            for (auto& c : opt.dto.cases)
            {
                if (c.name == "1-1")
                {
                    c.icon = "https://cdn.example.com/1-1.png";
                }
            }
        }
    }

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    MaapiPiTaskSettingsDto task;
    task.item_id = "item_icon";
    task.task_name = "DailyFarm";
    settings.pi.tasks.push_back(std::move(task));

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(result.valid)
        << "error_kind=" << result.error_kind << " message=" << result.message;

    auto obj = document.as_object();
    ASSERT_TRUE(obj.has_value());

    auto controller_field = FindRootField(document, "controllerName");
    ASSERT_TRUE(controller_field.has_value());
    {
        auto cands = controller_field->as_object()
                         .value()[std::string_view("candidates")]
                         .as_array();
        ASSERT_TRUE(cands.has_value());
        ASSERT_FALSE(cands->empty());
        auto first_cand = (*cands)[0].as_object();
        ASSERT_TRUE(first_cand.has_value());
        ASSERT_TRUE(first_cand->contains(std::string_view("icon")));
        EXPECT_EQ(
            (*first_cand)[std::string_view("icon")].as_string().value_or(""),
            "https://example.com/android.png");
    }

    auto ac = (*obj)[std::string_view("actionCatalog")].as_object();
    ASSERT_TRUE(ac.has_value());
    auto entries = (*ac)[std::string_view("entries")].as_array();
    ASSERT_TRUE(entries.has_value());
    std::string task_icon;
    for (auto it = entries->begin(); it != entries->end(); ++it)
    {
        auto entry = it->as_object();
        if (entry
            && (*entry)[std::string_view("actionId")].as_string().value_or("")
                   == "DailyFarm")
        {
            task_icon = std::string(
                (*entry)[std::string_view("icon")].as_string().value_or(""));
            break;
        }
    }
    EXPECT_EQ(task_icon, "C:/maapi/icons/daily.png");

    auto fs = (*obj)[std::string_view("formSequence")].as_object();
    ASSERT_TRUE(fs.has_value());
    auto items = (*fs)[std::string_view("items")].as_array();
    ASSERT_TRUE(items.has_value());
    auto item_obj = (*items)[0].as_object();
    ASSERT_TRUE(item_obj.has_value());
    auto item_fields = (*item_obj)[std::string_view("fields")].as_array();
    ASSERT_TRUE(item_fields.has_value());
    std::string stage_icon;
    std::string case_icon;
    for (auto it = item_fields->begin(); it != item_fields->end(); ++it)
    {
        auto field = it->as_object();
        if (!field)
        {
            continue;
        }
        if ((*field)[std::string_view("name")].as_string().value_or("")
            == "options.stage")
        {
            stage_icon = std::string(
                (*field)[std::string_view("icon")].as_string().value_or(""));
            auto cands = (*field)[std::string_view("candidates")].as_array();
            if (cands)
            {
                for (auto cit = cands->begin(); cit != cands->end(); ++cit)
                {
                    auto c = cit->as_object();
                    if (c
                        && (*c)[std::string_view("value")].as_string().value_or(
                               "")
                               == "1-1"
                        && c->contains(std::string_view("icon")))
                    {
                        case_icon = std::string(
                            (*c)[std::string_view("icon")].as_string().value_or(
                                ""));
                    }
                }
            }
            break;
        }
    }
    EXPECT_EQ(stage_icon, "C:/maapi/strict/icons/stage.png");
    EXPECT_EQ(case_icon, "https://cdn.example.com/1-1.png");
}

// ---------------------------------------------------------------------------
// Phase 84-07: 严格 Apply dispatch + accepted-result 语义 + Compile 精确化
//
// 这一组测试锁定 D-84-08/10/11/22/24/25/28/30/34/35/36/40 在 MaaPI Apply
// 上的最终严格 contract：
//   - setValue 严格 {target, fieldName, value}；旧 valuePath/nodeId+field 拒绝
//   - add/move/removeFormSequenceItem 按 itemId 寻址，duplicate task 类型独立
//   - invokeCommand 执行 new/copy；unknown command 返回诊断；无 revision 变化
//   - convertView 拒绝（MaaPI 只支持 formSequence）
//   - 成功 Apply 返回 {acceptedProperties, document, diagnostics}，
//     document.revision == baseRevision + 1
//   - 失败 Apply 不修改 acceptedProperties，revision 不变，仅 diagnostics
//   - Compile({}) 返回 {compile, diagnostics}，不持久化
//   - 旧
//   kind（applyPreset/addSequenceItem/moveSequenceItem/removeSequenceItem）
//     一律 unsupportedChange 拒绝
//   - 输出通过 84-01 ValidateAuthoringApplyResult /
//   ValidateAuthoringCompileResult
//
// 测试经公共 IDasTaskAuthoringSession 缝观察，literal JSON；不引用私有 helper。
// ---------------------------------------------------------------------------

// Helper: 取 Apply result envelope 的 acceptedProperties.pi 字段。
// 失败时返回空对象，便于 ASSERT 张口即停。
yyjson::value AcceptedPiFromApplyResult(const yyjson::value& result)
{
    auto obj = result.as_object();
    if (!obj)
    {
        return Das::Utils::MakeYyjsonObject();
    }
    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    if (!accepted)
    {
        return Das::Utils::MakeYyjsonObject();
    }
    auto pi = (*accepted)[std::string_view("pi")];
    return Das::Utils::CloneYyjsonValue(pi);
}

// Helper: 取 Apply result 的 revision（位于 document.revision）。
int64_t DocumentRevisionFromApplyResult(const yyjson::value& result)
{
    auto obj = result.as_object();
    if (!obj)
    {
        return -1;
    }
    if (!obj->contains(std::string_view("document")))
    {
        return -1;
    }
    auto document = (*obj)[std::string_view("document")].as_object();
    if (!document)
    {
        return -1;
    }
    return (*document)[std::string_view("revision")].as_int().value_or(-1);
}

// Helper: 统计 diagnostics 数组中给定 severity 的条数。
std::size_t CountDiagnosticsBySeverity(
    const yyjson::value& result,
    std::string_view     severity)
{
    auto obj = result.as_object();
    if (!obj || !obj->contains(std::string_view("diagnostics")))
    {
        return 0u;
    }
    auto arr = (*obj)[std::string_view("diagnostics")].as_array();
    if (!arr)
    {
        return 0u;
    }
    std::size_t count = 0u;
    for (auto it = arr->begin(); it != arr->end(); ++it)
    {
        auto diag = it->as_object();
        if (diag
            && (*diag)[std::string_view("severity")].as_string().value_or("")
                   == severity)
        {
            ++count;
        }
    }
    return count;
}

// Tracer 1 RED: setValue 严格 {target, fieldName, value} 形态。
// document 级 target 更新 controllerName 成功；revision 0 → 1。
TEST_F(MaapiAuthoringFixture, SetValueDocumentTargetControllerNameSucceeds)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
        "\"target\":{\"kind\":\"document\"},\"fieldName\":\"controllerName\","
        "\"value\":\"Android\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());

    // 严格 envelope 校验：通过 84-01 validator。
    auto apply_valid =
        Das::Core::TaskScheduler::ValidateAuthoringApplyResult(result, 0);
    EXPECT_TRUE(apply_valid.valid) << "error_kind=" << apply_valid.error_kind
                                   << " message=" << apply_valid.message;

    // controllerName 已写入 acceptedProperties。
    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    EXPECT_EQ(
        (*pi_obj)[std::string_view("controllerName")].as_string().value_or(""),
        "Android");

    // 成功 Apply 必须 document.revision == baseRevision + 1。
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 1);
    // 无 error severity 诊断。
    EXPECT_EQ(CountDiagnosticsBySeverity(result, "error"), 0u);
}

// Tracer 1 RED: setValue 严格 target=document 拒绝未知 fieldName。
// 不修改 acceptedProperties，不递增 revision。
TEST_F(MaapiAuthoringFixture, SetValueDocumentTargetRejectsUnknownField)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    // 先 Get 一份初始 document，记录 revision 0。
    DasPtr<Das::ExportInterface::IDasJson> get_json;
    ASSERT_EQ(session->GetDocument({}, get_json.Put()), DAS_S_OK);
    auto initial_doc = DocumentOfGet(get_json.Get());

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
        "\"target\":{\"kind\":\"document\"},\"fieldName\":\"bogusField\","
        "\"value\":\"x\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());

    // 必须有 error severity 诊断。
    EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);

    // revision 未递增：document.revision 仍是 0（保持 baseRevision）。
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);

    // acceptedProperties 不变（没有 bogus 字段被写入）。
    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    EXPECT_FALSE(pi_obj->contains(std::string_view("bogusField")));
}

// Tracer 1 RED: setValue 严格 target=formSequenceItem 更新 enabled。
TEST_F(MaapiAuthoringFixture, SetValueFormSequenceItemTargetUpdatesEnabled)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_a\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
        "\"target\":{\"kind\":\"formSequenceItem\",\"id\":\"item_a\"},"
        "\"fieldName\":\"enabled\",\"value\":false}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    auto tasks = (*pi_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    auto task = (*tasks)[0].as_object();
    ASSERT_TRUE(task.has_value());
    EXPECT_FALSE((*task)[std::string_view("enabled")].as_bool().value_or(true));
    // 成功 Apply 必须 document.revision == baseRevision + 1。
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 1);
    EXPECT_EQ(CountDiagnosticsBySeverity(result, "error"), 0u);
}

// Tracer 1 RED: setValue target=formSequenceItem 拒绝未知 id。
TEST_F(MaapiAuthoringFixture, SetValueFormSequenceItemTargetRejectsUnknownId)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_a\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
        "\"target\":{\"kind\":\"formSequenceItem\",\"id\":\"missing\"},"
        "\"fieldName\":\"enabled\",\"value\":false}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    // 必须有 error 诊断；revision 不变。
    EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);

    // acceptedProperties 未变：item_a 仍 enabled=true。
    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    auto tasks = (*pi_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    auto task = (*tasks)[0].as_object();
    ASSERT_TRUE(task.has_value());
    EXPECT_TRUE((*task)[std::string_view("enabled")].as_bool().value_or(false));
}

// Tracer 1 RED: setValue 拒绝 legacy valuePath payload（unsupportedChange）。
TEST_F(MaapiAuthoringFixture, SetValueRejectsLegacyValuePathPayload)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
        "\"valuePath\":\"controllerName\",\"value\":\"Android\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    // 旧 payload 必须被拒绝（error 诊断）。
    EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);

    // acceptedProperties 未被修改。
    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    EXPECT_FALSE(pi_obj->contains(std::string_view("controllerName")));
}

// Tracer 1 RED: setValue 拒绝 legacy nodeId+field
// payload（unsupportedChange）。
TEST_F(MaapiAuthoringFixture, SetValueRejectsLegacyNodeIdFieldPayload)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_a\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
        "\"nodeId\":\"DailyFarm\",\"field\":\"enabled\",\"value\":false}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);

    // acceptedProperties 未被修改：item_a 仍 enabled=true。
    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    auto tasks = (*pi_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    auto task = (*tasks)[0].as_object();
    ASSERT_TRUE(task.has_value());
    EXPECT_TRUE((*task)[std::string_view("enabled")].as_bool().value_or(false));
}

// Tracer 1 RED: setValue payload 出现额外字段时拒绝。
TEST_F(MaapiAuthoringFixture, SetValueRejectsExtraPayloadMember)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
        "\"target\":{\"kind\":\"document\"},\"fieldName\":\"controllerName\","
        "\"value\":\"Android\",\"extra\":\"nope\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);
}

// Tracer 2 RED: addFormSequenceItem 接受已注册 actionId，分配 itemId 与默认
// 字段；duplicate task 类型实例独立。document.revision == baseRevision + 1。
TEST_F(MaapiAuthoringFixture, AddFormSequenceItemRegistersTaskInstance)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"addFormSequenceItem\","
        "\"payload\":{\"actionId\":\"DailyFarm\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    // 成功 mutation：revision 1，无 error 诊断。
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 1);
    EXPECT_EQ(CountDiagnosticsBySeverity(result, "error"), 0u);

    // acceptedProperties.pi.tasks 已加入 DailyFarm 实例，带 Provider 颁发的
    // 非空 itemId。
    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    auto tasks = (*pi_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    auto task_obj = (*tasks)[0].as_object();
    ASSERT_TRUE(task_obj.has_value());
    EXPECT_EQ(
        (*task_obj)[std::string_view("taskName")].as_string().value_or(""),
        "DailyFarm");
    auto item_id =
        (*task_obj)[std::string_view("itemId")].as_string().value_or("");
    EXPECT_FALSE(item_id.empty());
}

// Tracer 2 RED: addFormSequenceItem 拒绝未注册的 actionId，不修改
// acceptedProperties，不递增 revision。
TEST_F(MaapiAuthoringFixture, AddFormSequenceItemRejectsUnknownActionId)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"addFormSequenceItem\","
        "\"payload\":{\"actionId\":\"NoSuchTask\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);

    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    auto tasks = (*pi_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    EXPECT_TRUE(tasks->empty());
}

// Tracer 2 RED: removeFormSequenceItem 通过 itemId 删除任务实例；同类型的
// 其它实例不受影响。
TEST_F(MaapiAuthoringFixture, RemoveFormSequenceItemDropsTaskByItemId)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":["
          "{\"itemId\":\"item_a\",\"taskName\":\"DailyFarm\",\"enabled\":true},"
          "{\"itemId\":\"item_b\",\"taskName\":\"DailyFarm\",\"enabled\":true}]"
          "}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"removeFormSequenceItem\","
        "\"payload\":{\"formSequenceItemId\":\"item_a\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 1);
    EXPECT_EQ(CountDiagnosticsBySeverity(result, "error"), 0u);

    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    auto tasks = (*pi_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
    auto remaining = (*tasks)[0].as_object();
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(
        (*remaining)[std::string_view("itemId")].as_string().value_or(""),
        "item_b");
}

// Tracer 2 RED: removeFormSequenceItem 拒绝未知 itemId，不递增 revision。
TEST_F(MaapiAuthoringFixture, RemoveFormSequenceItemRejectsUnknownId)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":["
          "{\"itemId\":\"item_a\",\"taskName\":\"DailyFarm\",\"enabled\":true}]"
          "}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"removeFormSequenceItem\","
        "\"payload\":{\"formSequenceItemId\":\"missing\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);

    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    auto tasks = (*pi_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 1u);
}

// Tracer 2 RED: moveFormSequenceItem 通过 itemId + destinationIndex 移动。
TEST_F(MaapiAuthoringFixture, MoveFormSequenceItemReordersByItemId)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"tasks\":["
          "{\"itemId\":\"item_a\",\"taskName\":\"DailyFarm\",\"enabled\":true},"
          "{\"itemId\":\"item_b\",\"taskName\":\"DailyFarm\",\"enabled\":true},"
          "{\"itemId\":\"item_c\",\"taskName\":\"DailyFarm\",\"enabled\":true}]"
          "}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"moveFormSequenceItem\","
        "\"payload\":{\"formSequenceItemId\":\"item_c\","
        "\"destinationIndex\":0}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 1);
    EXPECT_EQ(CountDiagnosticsBySeverity(result, "error"), 0u);

    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    auto tasks = (*pi_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 3u);
    auto first = (*tasks)[0].as_object();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(
        (*first)[std::string_view("itemId")].as_string().value_or(""),
        "item_c");
}

// Tracer 3 RED: invokeCommand 未知 commandId 必须拒绝，不递增 revision，
// 携带 error 诊断。
TEST_F(MaapiAuthoringFixture, InvokeCommandRejectsUnknownCommandId)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},\"pi\":{\"presetName\":\"DailyPreset\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"noSuchCommand\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);
    // Provider-rejected apply envelope (error diagnostics + revision not
    // bumped) must pass the strict Apply validator so the real reason surfaces
    // instead of revisionMismatch.
    auto rejected_valid =
        Das::Core::TaskScheduler::ValidateAuthoringApplyResult(result, 0);
    EXPECT_TRUE(rejected_valid.valid)
        << "error_kind=" << rejected_valid.error_kind
        << " message=" << rejected_valid.message;
}

// D-84.2-04: newPreset 是 D-84-28 通用 invokeCommand 在 MaaPI 的落地。
// flat pi seed（无 configurations）+ invokeCommand{commandId:"newPreset"} 应：
// (a) ApplyChange 返回 DAS_S_OK；(b) 通过 84-01 严格 Apply validator；
// (c) document.revision == 1；(d) 无 error 诊断；
// (e) acceptedProperties.pi.customPresets 从 0 变 1，新条目含非空 id/name；
// (f) 新条目镜像当前 flat pi 的 controller/resource/language；
// (g) document.commands[] 含 id="newPreset" 的 invokeCommand 条目；
// (h) acceptedProperties.pi.presetName 保持原值（newPreset 不污染 PI 官方
// preset 引用）。
TEST_F(MaapiAuthoringFixture, InvokeCommandNewPresetCreatesSnapshot)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    // flat pi seed：无 configurations，只有 PI 官方字段 + 一个 task 实例。
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\"},"
          "\"pi\":{\"controllerName\":\"Android\","
          "\"resourceName\":\"Official\",\"presetName\":\"DailyPreset\","
          "\"language\":\"zh_cn\",\"tasks\":[{\"itemId\":\"seed_t1\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"newPreset\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    // (b) 严格 envelope 校验。
    auto apply_valid =
        Das::Core::TaskScheduler::ValidateAuthoringApplyResult(result, 0);
    EXPECT_TRUE(apply_valid.valid) << "error_kind=" << apply_valid.error_kind
                                   << " message=" << apply_valid.message;
    // (c) 成功 Apply 必须 document.revision == baseRevision + 1。
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 1);
    // (d) 无 error severity 诊断。
    EXPECT_EQ(CountDiagnosticsBySeverity(result, "error"), 0u);

    // (e)(f)(h) acceptedProperties.pi 断言。
    auto pi = AcceptedPiFromApplyResult(result);
    auto pi_obj = pi.as_object();
    ASSERT_TRUE(pi_obj.has_value());
    // (h) newPreset 自动选中新 preset：presetName 应切到新 snapshot id
    // （在下方 snapshot_id 取得后断言）。
    // (e) customPresets 数组从 0 变 1。
    ASSERT_TRUE(pi_obj->contains(std::string_view("customPresets")));
    auto presets = (*pi_obj)[std::string_view("customPresets")].as_array();
    ASSERT_TRUE(presets.has_value());
    ASSERT_EQ(presets->size(), 1u);
    auto snapshot_obj = (*presets)[0].as_object();
    ASSERT_TRUE(snapshot_obj.has_value());
    auto snapshot_id =
        (*snapshot_obj)[std::string_view("id")].as_string().value_or("");
    auto snapshot_name =
        (*snapshot_obj)[std::string_view("name")].as_string().value_or("");
    EXPECT_FALSE(snapshot_id.empty());
    EXPECT_FALSE(snapshot_name.empty());
    // (h) newPreset 自动选中新 preset：presetName 切到新 snapshot id，
    // 经 apply-returns-document 反馈通道产生可见变化。
    EXPECT_EQ(
        (*pi_obj)[std::string_view("presetName")].as_string().value_or(""),
        snapshot_id);
    // (f) snapshot 镜像当前 flat pi 的 controller/resource/language。
    EXPECT_EQ(
        (*snapshot_obj)[std::string_view("controllerName")]
            .as_string()
            .value_or(""),
        "Android");
    EXPECT_EQ(
        (*snapshot_obj)[std::string_view("resourceName")].as_string().value_or(
            ""),
        "Official");
    EXPECT_EQ(
        (*snapshot_obj)[std::string_view("language")].as_string().value_or(""),
        "zh_cn");

    // (g) document.commands[] 含 newPreset invokeCommand 条目。
    auto result_obj = result.as_object();
    ASSERT_TRUE(result_obj.has_value());
    ASSERT_TRUE(result_obj->contains(std::string_view("document")));
    auto document = Das::Utils::CloneYyjsonValue(
        (*result_obj)[std::string_view("document")]);
    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    ASSERT_TRUE(doc_obj->contains(std::string_view("commands")));
    auto commands = (*doc_obj)[std::string_view("commands")].as_array();
    ASSERT_TRUE(commands.has_value());
    bool found_new_preset = false;
    for (auto it = commands->begin(); it != commands->end(); ++it)
    {
        auto cmd = it->as_object();
        if (!cmd)
        {
            continue;
        }
        if ((*cmd)[std::string_view("id")].as_string().value_or("")
            == "newPreset")
        {
            found_new_preset = true;
            auto apply = (*cmd)[std::string_view("apply")].as_object();
            ASSERT_TRUE(apply.has_value());
            EXPECT_EQ(
                (*apply)[std::string_view("kind")].as_string().value_or(""),
                "invokeCommand");
            auto payload = (*apply)[std::string_view("payload")].as_object();
            ASSERT_TRUE(payload.has_value());
            EXPECT_EQ(
                (*payload)[std::string_view("commandId")].as_string().value_or(
                    ""),
                "newPreset");
            break;
        }
    }
    EXPECT_TRUE(found_new_preset)
        << "document.commands[] missing newPreset entry";
}

// Tracer 3 RED: convertView 在 MaaPI 必须拒绝（MaaPI 只支持 formSequence）。
TEST_F(MaapiAuthoringFixture, ConvertViewRejectedByMaapi)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"convertView\","
        "\"payload\":{\"targetViewKind\":\"graph\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);
}

// Tracer 3 RED: legacy kinds（applyPreset / addSequenceItem /
// removeSequenceItem）一律 unsupportedChange 拒绝，不递增 revision。
TEST_F(MaapiAuthoringFixture, LegacyKindsRejectedAsUnsupportedChange)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    for (const auto* kind_str :
         {"applyPreset",
          "addSequenceItem",
          "moveSequenceItem",
          "removeSequenceItem",
          "addNode",
          "removeNode",
          "connectPorts",
          "disconnectPorts",
          "updateNodeConfig"})
    {
        DasPtr<Das::ExportInterface::IDasJson> result_json;
        const std::string                      change_json =
            std::string("{\"baseRevision\":0,\"kind\":\"") + kind_str
            + "\",\"payload\":{}}";
        auto change = ParseDasJson(change_json);
        ASSERT_EQ(
            session->ApplyChange(change.Get(), result_json.Put()),
            DAS_S_OK);
        auto result = ReadJsonInterface(result_json.Get());
        EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u)
            << "kind=" << kind_str;
        EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0)
            << "kind=" << kind_str;
    }
}

// Tracer 4 RED: Compile({}) 返回严格 {compile, diagnostics} envelope，通过
// 84-01 ValidateAuthoringCompileResult 校验，不持久化。
TEST_F(MaapiAuthoringFixture, CompileEmptyRequestReturnsStrictEnvelope)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    ASSERT_EQ(session->Compile({}, result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    // 严格 Compile envelope 校验。
    auto compile_valid =
        Das::Core::TaskScheduler::ValidateAuthoringCompileResult(result);
    EXPECT_TRUE(compile_valid.valid)
        << "error_kind=" << compile_valid.error_kind
        << " message=" << compile_valid.message;
}

// Tracer 4 RED: 成功 Apply envelope 通过 84-01 validator。
TEST(MaapiApplyResultTest, SuccessfulApplyEnvelopePassesStrictValidator)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    // D-84.2-04: 通过 ApplyInvokeCommandChange(newPreset) 触发一次成功
    // mutation。
    AcceptedSettingsDto candidate = settings;
    auto                new_preset_payload =
        Das::Utils::ParseYyjsonFromString(R"({"commandId":"newPreset"})");
    ASSERT_TRUE(new_preset_payload.has_value());
    std::vector<PiDiagnosticDto> invoke_diagnostics;
    ASSERT_TRUE(ApplyInvokeCommandChange(
        candidate,
        *new_preset_payload,
        invoke_diagnostics));

    std::vector<PiDiagnosticDto> diagnostics;
    auto result = Das::Plugins::DasMaaPi::BuildApplyResult(
        candidate,
        /*revision=*/1,
        diagnostics);

    auto apply_valid =
        Das::Core::TaskScheduler::ValidateAuthoringApplyResult(result, 0);
    EXPECT_TRUE(apply_valid.valid) << "error_kind=" << apply_valid.error_kind
                                   << " message=" << apply_valid.message;

    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE(obj->contains(std::string_view("acceptedProperties")));
    EXPECT_TRUE(obj->contains(std::string_view("document")));
    EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));
    EXPECT_FALSE(obj->contains(std::string_view("sourceFingerprint")));
    EXPECT_FALSE(obj->contains(std::string_view("migration")));
    EXPECT_FALSE(obj->contains(std::string_view("dynamic_ports")));
}

// ============================================================================
// Phase 84-08 — Backend end-to-end regression on disposable post-84 state.
//
// Tests in this section prove the completed backend cutover (84-01 strict
// validators + 84-02 GraphRuntime registry actionCatalog + 84-03 Scheduler
// transparent coordinator + 84-04 manifest PropertySchema + 84-05 MaaPI
// configuration authority + 84-06 MaaPI strict Get projection + 84-07 MaaPI
// strict Apply/Compile dispatch) mutually agree on the wire format end to
// end. They drive the public MaaPI session seam with INDEPENDENT LITERAL
// fixtures (interface_authoring_v28.jsonc and post84_accepted_properties.json)
// — never with expected data produced by the production serializer — and
// cross-check every envelope against the 84-01 strict validators.
//
// They also lock the read-only boundary around the gitignored live legacy
// sample build/mingw-debug/bin/Debug/settings/0/taskId7.json. That file
// predates stable itemId and is owned by Phase 84.1. The gate is a pre/post
// SHA-256 comparison of the on-disk bytes, NOT `git diff --exit-code`, because
// the path is gitignored and `git diff --exit-code` would always report
// "no diff" — even after mutation. The SHA-256 implementation is inline
// (public domain, Brad Conte sha-256) so the test does not depend on a
// crypto library that is not otherwise part of the project.
// ============================================================================

namespace
{
    // Minimal SHA-256 (public domain, Brad Conte). One-shot interface; this
    // is a forensic-equality gate, not a hardened crypto boundary, but we use
    // the real algorithm so a third party can independently recompute hashes
    // from `sha256sum build/.../taskId7.json` and compare against the values
    // the test logs.
    class Sha256
    {
    public:
        Sha256() = default;

        explicit Sha256(std::string_view data)
        {
            Update(data);
            Finalize();
        }

        void Update(std::string_view data)
        {
            for (std::size_t i = 0; i < data.size(); ++i)
            {
                data_[datalen_++] = static_cast<std::uint8_t>(data[i]);
                if (datalen_ == 64)
                {
                    Transform();
                    bitlen_ += 512;
                    datalen_ = 0;
                }
            }
        }

        void Finalize()
        {
            std::uint64_t i = datalen_;
            // Pad whatever data is left in the buffer.
            if (datalen_ < 56)
            {
                data_[i++] = 0x80;
                while (i < 56)
                {
                    data_[i++] = 0x00;
                }
            }
            else
            {
                data_[i++] = 0x80;
                while (i < 64)
                {
                    data_[i++] = 0x00;
                }
                Transform();
                std::memset(data_.data(), 0, 56);
            }
            bitlen_ += static_cast<std::uint64_t>(datalen_) * 8;
            data_[63] = static_cast<std::uint8_t>(bitlen_);
            data_[62] = static_cast<std::uint8_t>(bitlen_ >> 8);
            data_[61] = static_cast<std::uint8_t>(bitlen_ >> 16);
            data_[60] = static_cast<std::uint8_t>(bitlen_ >> 24);
            data_[59] = static_cast<std::uint8_t>(bitlen_ >> 32);
            data_[58] = static_cast<std::uint8_t>(bitlen_ >> 40);
            data_[57] = static_cast<std::uint8_t>(bitlen_ >> 48);
            data_[56] = static_cast<std::uint8_t>(bitlen_ >> 56);
            Transform();
        }

        std::array<std::uint8_t, 32> Digest() const
        {
            std::array<std::uint8_t, 32> out{};
            for (std::size_t i = 0; i < 4; ++i)
            {
                out[i] = static_cast<std::uint8_t>(
                    (state_[0] >> (24 - i * 8)) & 0xff);
                out[i + 4] = static_cast<std::uint8_t>(
                    (state_[1] >> (24 - i * 8)) & 0xff);
                out[i + 8] = static_cast<std::uint8_t>(
                    (state_[2] >> (24 - i * 8)) & 0xff);
                out[i + 12] = static_cast<std::uint8_t>(
                    (state_[3] >> (24 - i * 8)) & 0xff);
                out[i + 16] = static_cast<std::uint8_t>(
                    (state_[4] >> (24 - i * 8)) & 0xff);
                out[i + 20] = static_cast<std::uint8_t>(
                    (state_[5] >> (24 - i * 8)) & 0xff);
                out[i + 24] = static_cast<std::uint8_t>(
                    (state_[6] >> (24 - i * 8)) & 0xff);
                out[i + 28] = static_cast<std::uint8_t>(
                    (state_[7] >> (24 - i * 8)) & 0xff);
            }
            return out;
        }

        std::string Hex() const
        {
            static constexpr char kHex[] = "0123456789abcdef";
            auto                  d = Digest();
            std::string           s(d.size() * 2, '0');
            for (std::size_t i = 0; i < d.size(); ++i)
            {
                s[i * 2] = kHex[(d[i] >> 4) & 0xf];
                s[i * 2 + 1] = kHex[d[i] & 0xf];
            }
            return s;
        }

    private:
        static constexpr std::uint32_t K0 = 0x428a2f98;
        static constexpr std::uint32_t K1 = 0x71374491;
        static constexpr std::uint32_t K2 = 0xb5c0fbcf;
        static constexpr std::uint32_t K3 = 0xe9b5dba5;
        static constexpr std::uint32_t K4 = 0x3956c25b;
        static constexpr std::uint32_t K5 = 0x59f111f1;
        static constexpr std::uint32_t K6 = 0x923f82a4;
        static constexpr std::uint32_t K7 = 0xab1c5ed5;
        static constexpr std::uint32_t K8 = 0xd807aa98;
        static constexpr std::uint32_t K9 = 0x12835b01;
        static constexpr std::uint32_t K10 = 0x243185be;
        static constexpr std::uint32_t K11 = 0x550c7dc3;
        static constexpr std::uint32_t K12 = 0x72be5d74;
        static constexpr std::uint32_t K13 = 0x80deb1fe;
        static constexpr std::uint32_t K14 = 0x9bdc06a7;
        static constexpr std::uint32_t K15 = 0xc19bf174;
        static constexpr std::uint32_t K16 = 0xe49b69c1;
        static constexpr std::uint32_t K17 = 0xefbe4786;
        static constexpr std::uint32_t K18 = 0x0fc19dc6;
        static constexpr std::uint32_t K19 = 0x240ca1cc;
        static constexpr std::uint32_t K20 = 0x2de92c6f;
        static constexpr std::uint32_t K21 = 0x4a7484aa;
        static constexpr std::uint32_t K22 = 0x5cb0a9dc;
        static constexpr std::uint32_t K23 = 0x76f988da;
        static constexpr std::uint32_t K24 = 0x983e5152;
        static constexpr std::uint32_t K25 = 0xa831c66d;
        static constexpr std::uint32_t K26 = 0xb00327c8;
        static constexpr std::uint32_t K27 = 0xbf597fc7;
        static constexpr std::uint32_t K28 = 0xc6e00bf3;
        static constexpr std::uint32_t K29 = 0xd5a79147;
        static constexpr std::uint32_t K30 = 0x06ca6351;
        static constexpr std::uint32_t K31 = 0x14292967;
        static constexpr std::uint32_t K32 = 0x27b70a85;
        static constexpr std::uint32_t K33 = 0x2e1b2138;
        static constexpr std::uint32_t K34 = 0x4d2c6dfc;
        static constexpr std::uint32_t K35 = 0x53380d13;
        static constexpr std::uint32_t K36 = 0x650a7354;
        static constexpr std::uint32_t K37 = 0x766a0abb;
        static constexpr std::uint32_t K38 = 0x81c2c92e;
        static constexpr std::uint32_t K39 = 0x92722c85;
        static constexpr std::uint32_t K40 = 0xa2bfe8a1;
        static constexpr std::uint32_t K41 = 0xa81a664b;
        static constexpr std::uint32_t K42 = 0xc24b8b70;
        static constexpr std::uint32_t K43 = 0xc76c51a3;
        static constexpr std::uint32_t K44 = 0xd192e819;
        static constexpr std::uint32_t K45 = 0xd6990624;
        static constexpr std::uint32_t K46 = 0xf40e3585;
        static constexpr std::uint32_t K47 = 0x106aa070;
        static constexpr std::uint32_t K48 = 0x19a4c116;
        static constexpr std::uint32_t K49 = 0x1e376c08;
        static constexpr std::uint32_t K50 = 0x2748774c;
        static constexpr std::uint32_t K51 = 0x34b0bcb5;
        static constexpr std::uint32_t K52 = 0x391c0cb3;
        static constexpr std::uint32_t K53 = 0x4ed8aa4a;
        static constexpr std::uint32_t K54 = 0x5b9cca4f;
        static constexpr std::uint32_t K55 = 0x682e6ff3;
        static constexpr std::uint32_t K56 = 0x748f82ee;
        static constexpr std::uint32_t K57 = 0x78a5636f;
        static constexpr std::uint32_t K58 = 0x84c87814;
        static constexpr std::uint32_t K59 = 0x8cc70208;
        static constexpr std::uint32_t K60 = 0x90befffa;
        static constexpr std::uint32_t K61 = 0xa4506ceb;
        static constexpr std::uint32_t K62 = 0xbef9a3f7;
        static constexpr std::uint32_t K63 = 0xc67178f2;

        static std::uint32_t Rotr(std::uint32_t x, std::uint32_t n)
        {
            return (x >> n) | (x << (32 - n));
        }
        static std::uint32_t Sigma0(std::uint32_t x)
        {
            return Rotr(x, 7) ^ Rotr(x, 18) ^ (x >> 3);
        }
        static std::uint32_t Sigma1(std::uint32_t x)
        {
            return Rotr(x, 17) ^ Rotr(x, 19) ^ (x >> 10);
        }
        static std::uint32_t BigSigma0(std::uint32_t x)
        {
            return Rotr(x, 2) ^ Rotr(x, 13) ^ Rotr(x, 22);
        }
        static std::uint32_t BigSigma1(std::uint32_t x)
        {
            return Rotr(x, 6) ^ Rotr(x, 11) ^ Rotr(x, 25);
        }
        static std::uint32_t Ch(
            std::uint32_t x,
            std::uint32_t y,
            std::uint32_t z)
        {
            return (x & y) ^ (~x & z);
        }
        static std::uint32_t Maj(
            std::uint32_t x,
            std::uint32_t y,
            std::uint32_t z)
        {
            return (x & y) ^ (x & z) ^ (y & z);
        }

        void Transform()
        {
            std::uint32_t m[64];
            for (std::size_t i = 0, j = 0; i < 16; ++i, j += 4)
            {
                m[i] = (static_cast<std::uint32_t>(data_[j]) << 24)
                       | (static_cast<std::uint32_t>(data_[j + 1]) << 16)
                       | (static_cast<std::uint32_t>(data_[j + 2]) << 8)
                       | (static_cast<std::uint32_t>(data_[j + 3]));
            }
            for (std::size_t i = 16; i < 64; ++i)
            {
                m[i] =
                    Sigma1(m[i - 2]) + m[i - 7] + Sigma0(m[i - 15]) + m[i - 16];
            }
            std::uint32_t                  a = state_[0];
            std::uint32_t                  b = state_[1];
            std::uint32_t                  c = state_[2];
            std::uint32_t                  d = state_[3];
            std::uint32_t                  e = state_[4];
            std::uint32_t                  f = state_[5];
            std::uint32_t                  g = state_[6];
            std::uint32_t                  h = state_[7];
            static constexpr std::uint32_t k[64] = {
                K0,  K1,  K2,  K3,  K4,  K5,  K6,  K7,  K8,  K9,  K10, K11, K12,
                K13, K14, K15, K16, K17, K18, K19, K20, K21, K22, K23, K24, K25,
                K26, K27, K28, K29, K30, K31, K32, K33, K34, K35, K36, K37, K38,
                K39, K40, K41, K42, K43, K44, K45, K46, K47, K48, K49, K50, K51,
                K52, K53, K54, K55, K56, K57, K58, K59, K60, K61, K62, K63};
            for (std::size_t i = 0; i < 64; ++i)
            {
                std::uint32_t t1 = h + BigSigma1(e) + Ch(e, f, g) + k[i] + m[i];
                std::uint32_t t2 = BigSigma0(a) + Maj(a, b, c);
                h = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            state_[0] += a;
            state_[1] += b;
            state_[2] += c;
            state_[3] += d;
            state_[4] += e;
            state_[5] += f;
            state_[6] += g;
            state_[7] += h;
        }

        std::array<std::uint8_t, 64> data_{};
        std::uint64_t                datalen_{0};
        std::uint64_t                bitlen_{0};
        std::array<std::uint32_t, 8> state_{
            0x6a09e667,
            0xbb67ae85,
            0x3c6ef372,
            0xa54ff53a,
            0x510e527f,
            0x9b05688c,
            0x1f83d9ab,
            0x5be0cd19};
    };

    // 读 fixtures 字面量为 std::string，避免测试依赖生产 serializer 生成期望。
    std::optional<std::string> ReadFixtureText(std::string_view relative)
    {
        std::ifstream ifs(FixturePath(std::string(relative)), std::ios::binary);
        if (!ifs.is_open())
        {
            return std::nullopt;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        return oss.str();
    }

    // 将 fixture 文本（带可选 JSONC // 注释）剥成纯 JSON 文本。仅剔除整行
    // // 注释，不改变 token 边界，足够满足现有 fixtures 的格式。
    std::string StripJsoncLineComments(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        std::size_t i = 0;
        while (i < text.size())
        {
            // 行内 // 注释（不识别字符串字面量内部的 //，因为现有 fixtures
            // 不包含这样的字符串）。
            if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/')
            {
                while (i < text.size() && text[i] != '\n')
                {
                    ++i;
                }
                continue;
            }
            out.push_back(text[i]);
            ++i;
        }
        return out;
    }

    std::optional<yyjson::value> ParseJsonFromStringStrict(
        std::string_view text)
    {
        auto stripped = StripJsoncLineComments(text);
        auto parsed = Das::Utils::ParseYyjsonFromString(stripped);
        if (!parsed)
        {
            return std::nullopt;
        }
        return std::move(*parsed);
    }

    // 构造一份 literal context：把 fixture 内 adapter + pi 字段直接装进
    // session context JSON（顶层）。等价于 SchedulerService 把
    // acceptedProperties 摊平到 context 的形态，避免测试依赖 scheduler。
    std::string BuildSessionContextFromFixture(std::string_view fixture_text)
    {
        auto parsed = ParseJsonFromStringStrict(fixture_text);
        if (!parsed)
        {
            return "{}";
        }
        auto obj = parsed->as_object();
        if (!obj)
        {
            return "{}";
        }
        // 取 adapter 与 pi（可选）。
        std::string context = "{";
        if (obj->contains(std::string_view("adapter")))
        {
            auto adapter = (*obj)[std::string_view("adapter")];
            // Use yyjson serialization to avoid manual escaping.
            auto adapter_str =
                Das::Utils::SerializeYyjsonValue(adapter, /*pretty=*/false);
            context += "\"adapter\":";
            context += adapter_str.value_or("{}");
        }
        if (obj->contains(std::string_view("pi")))
        {
            auto pi = (*obj)[std::string_view("pi")];
            if (obj->contains(std::string_view("adapter")))
            {
                context += ",";
            }
            auto pi_str =
                Das::Utils::SerializeYyjsonValue(pi, /*pretty=*/false);
            context += "\"pi\":";
            context += pi_str.value_or("{}");
        }
        context += "}";
        return context;
    }

    // 把 yyjson value 序列化成 JSON 文本。失败返回 "{}"，避免测试因序列化
    // 异常崩溃。
    std::string SerializeOrEmpty(const yyjson::value& v)
    {
        auto s = Das::Utils::SerializeYyjsonValue(v, /*pretty=*/false);
        return s.value_or("{}");
    }

    // 从 document.formSequence.items[] 收集所有 id（FormSequenceItem.id =
    // itemId）。
    std::vector<std::string> CollectFormItemIds(const yyjson::value& document)
    {
        std::vector<std::string> ids;
        auto                     obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("formSequence")))
        {
            return ids;
        }
        auto fs = (*obj)[std::string_view("formSequence")].as_object();
        if (!fs || !fs->contains(std::string_view("items")))
        {
            return ids;
        }
        auto items = (*fs)[std::string_view("items")].as_array();
        if (!items)
        {
            return ids;
        }
        for (auto it = items->begin(); it != items->end(); ++it)
        {
            auto item = it->as_object();
            if (!item || !item->contains(std::string_view("id")))
            {
                continue;
            }
            ids.emplace_back(
                (*item)[std::string_view("id")].as_string().value_or(""));
        }
        return ids;
    }

    // 从 document.actionCatalog.entries[] 收集 actionId 集合。
    std::vector<std::string> CollectActionIds(const yyjson::value& document)
    {
        std::vector<std::string> ids;
        auto                     obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("actionCatalog")))
        {
            return ids;
        }
        auto ac = (*obj)[std::string_view("actionCatalog")].as_object();
        if (!ac || !ac->contains(std::string_view("entries")))
        {
            return ids;
        }
        auto entries = (*ac)[std::string_view("entries")].as_array();
        if (!entries)
        {
            return ids;
        }
        for (auto it = entries->begin(); it != entries->end(); ++it)
        {
            auto entry = it->as_object();
            if (!entry || !entry->contains(std::string_view("actionId")))
            {
                continue;
            }
            ids.emplace_back(
                (*entry)[std::string_view("actionId")].as_string().value_or(
                    ""));
        }
        return ids;
    }

    // 取 document.revision。
    int64_t DocumentRevision(const yyjson::value& document)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("revision")))
        {
            return -1;
        }
        return (*obj)[std::string_view("revision")].as_int().value_or(-1);
    }

    // 从 Apply result envelope 取出 document 字段（独立 value，安全）。
    yyjson::value DocumentFromApplyResult(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj || !obj->contains(std::string_view("document")))
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("document")]);
    }

    // 计算 taskId7.json 工作目录相对路径（ctest 工作目录是
    // ${CMAKE_BINARY_DIR}/Test；live 文件在
    // ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/settings/0/taskId7.json）。
    std::filesystem::path LegacyTaskId7Path()
    {
        return std::filesystem::current_path() / ".." / "bin" / "Debug"
               / "settings" / "0" / "taskId7.json";
    }

    // 读任意文件（二进制）为 std::string。
    std::optional<std::string> ReadFileBytes(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            return std::nullopt;
        }
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open())
        {
            return std::nullopt;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        return oss.str();
    }
} // namespace

// Tracer 1 RED: 字面量 v28 PI → MaaPI Provider Get → 严格 84-01 Get validator
// 通过。 actionCatalog 必须同时包含可重复的 PI task 类型（D-84-40 同 actionId
// 可多次添加）。 formSequence.items 为空（fresh state）但 form view 已经定形。
TEST_F(MaapiAuthoringFixture, LiteralV28InterfaceProjectsStrictWire)
{
    auto fixture_text = ReadFixtureText("interface_authoring_v28.jsonc");
    ASSERT_TRUE(fixture_text.has_value()) << "fixture missing";
    auto interface_path =
        FixturePath("interface_authoring_v28.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> get_json;
    ASSERT_EQ(session->GetDocument({}, get_json.Put()), DAS_S_OK);
    // Production-path: Provider GetDocument must return the strict
    // {document, diagnostics} envelope directly. A prior regression returned
    // a bare document and SchedulerService rejected it with
    // "AuthoringGetResult.diagnostics required".
    auto envelope = ReadJsonInterface(get_json.Get());
    auto get_valid =
        Das::Core::TaskScheduler::ValidateAuthoringGetResult(envelope);
    EXPECT_TRUE(get_valid.valid) << "error_kind=" << get_valid.error_kind
                                 << " message=" << get_valid.message;

    auto document = DocumentOfGet(get_json.Get());
    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    EXPECT_EQ(
        (*doc_obj)[std::string_view("viewKind")].as_string().value_or(""),
        "formSequence");
    EXPECT_EQ(
        (*doc_obj)[std::string_view("contractVersion")].as_int().value_or(0),
        1);
    EXPECT_FALSE(doc_obj->contains(std::string_view("kind")));
    EXPECT_FALSE(doc_obj->contains(std::string_view("catalog")));
    EXPECT_FALSE(doc_obj->contains(std::string_view("sourceFingerprint")));

    // actionCatalog 必须暴露两个可创建的 PI task 类型。
    auto action_ids = CollectActionIds(document);
    EXPECT_EQ(action_ids.size(), 2u);
    bool has_daily_farm = false;
    bool has_bonus_task = false;
    for (const auto& id : action_ids)
    {
        if (id == "DailyFarm")
        {
            has_daily_farm = true;
        }
        if (id == "DailyFarmBonus")
        {
            has_bonus_task = true;
        }
    }
    EXPECT_TRUE(has_daily_farm);
    EXPECT_TRUE(has_bonus_task);

    // 预期一次性字段：retry.times 与 retry.unit（D-84-17 multi-input 拆分）。
    // 在 fresh state（空 acceptedProperties.pi.tasks）下 formSequence.items
    // 为空。
    auto item_ids = CollectFormItemIds(document);
    EXPECT_EQ(item_ids.size(), 0u);
}

// UAT 端到端回归：用户在 UI 创建 MaaPI 任务（fresh session，空
// acceptedProperties.pi.global_options）→ GetDocument → document.fields[]
// 必须含 PI 接口定义的默认静态设置。stage 在 v28 fixture 中是
// global_option，select 类型，cases=[1-1, 1-2]，无 default_case，按
// MaaPI 约定默认回退首个 case "1-1"。
TEST_F(MaapiAuthoringFixture, FreshSessionProjectsGlobalOptionPiDefault)
{
    auto interface_path =
        FixturePath("interface_authoring_v28.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> get_json;
    ASSERT_EQ(session->GetDocument({}, get_json.Put()), DAS_S_OK);
    auto envelope = ReadJsonInterface(get_json.Get());
    auto get_valid =
        Das::Core::TaskScheduler::ValidateAuthoringGetResult(envelope);
    EXPECT_TRUE(get_valid.valid) << "error_kind=" << get_valid.error_kind
                                 << " message=" << get_valid.message;

    auto document = DocumentOfGet(get_json.Get());
    // stage 是 v28 fixture 的唯一 global_option；fresh session 下
    // settings.pi.global_options 为空，必须回退到 PI 首个 case "1-1"。
    auto stage_field = FindRootField(document, "options.stage");
    ASSERT_TRUE(stage_field.has_value());
    auto stage_obj = stage_field->as_object();
    ASSERT_TRUE(stage_obj.has_value());
    ASSERT_TRUE(stage_obj->contains(std::string_view("value")));
    auto stage_value =
        (*stage_obj)[std::string_view("value")].as_string().value_or("");
    EXPECT_EQ(stage_value, "1-1")
        << "global_option stage 必须回退到 PI 首个 case '";
}

// UAT 端到端回归：用户通过 addFormSequenceItem 在 UI 加入 DailyFarm 后，
// Apply result 的 document.formSequence.items[0].fields[] 必须含 PI 接口
// 默认值（retry multi-input 的 times=3 / unit=second；stage=1-1 首个 case；
// use-medicine=[] 因 v28 fixture 无 default_case）。
TEST_F(MaapiAuthoringFixture, AddFormSequenceItemProjectsTaskOptionPiDefaults)
{
    auto interface_path =
        FixturePath("interface_authoring_v28.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> apply_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"addFormSequenceItem\",\"payload\":{"
        "\"actionId\":\"DailyFarm\"}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), apply_json.Put()), DAS_S_OK);

    auto apply = ReadJsonInterface(apply_json.Get());
    auto apply_valid =
        Das::Core::TaskScheduler::ValidateAuthoringApplyResult(apply, 0);
    EXPECT_TRUE(apply_valid.valid) << "error_kind=" << apply_valid.error_kind
                                   << " message=" << apply_valid.message;

    auto document = DocumentFromApplyResult(apply);
    auto item_ids = CollectFormItemIds(document);
    ASSERT_EQ(item_ids.size(), 1u);

    // retry 是 multi-input (times/unit)，sub-field 寻址 options.retry.times。
    // times 的 PI default_value="3" (v28 fixture)，pipeline_type=int → 标量 3。
    // MakeInputSubField 经 mutable_value_base operator= 路径赋值，positive int
    // 可能落 uint subtype，故用 is_int() / as_int() 兼容两种 subtype。
    auto retry_times_val = FindSequenceItemFieldValue(
        document,
        item_ids[0],
        "options.retry.times");
    ASSERT_TRUE(retry_times_val.is_int());
    EXPECT_EQ(retry_times_val.as_int().value_or(0), 3);

    // unit 的 PI default_value="second" (v28 fixture)，pipeline_type=string →
    // string "second"。
    auto retry_unit_val =
        FindSequenceItemFieldValue(document, item_ids[0], "options.retry.unit");
    ASSERT_TRUE(retry_unit_val.is_string());
    EXPECT_EQ(retry_unit_val.as_string().value_or(""), "second");

    // stage 是 task 级 select，无 default_case → 回退首个 case "1-1"。
    // 注意：stage 也是 global_option，但在 task 级是 DailyFarm.dto.option
    // 之一。
    auto stage_val =
        FindSequenceItemFieldValue(document, item_ids[0], "options.stage");
    ASSERT_TRUE(stage_val.is_string());
    EXPECT_EQ(stage_val.as_string().value_or(""), "1-1");
}

// Tracer 1 RED: 字面量 post-84 state → Get→Apply→new-session Get 全链路
// 在 84-01 validator 下通过，itemId 稳定。证明 Provider → 投影 端到端正确。
TEST_F(MaapiAuthoringFixture, LiteralPost84StateRoundTripsProviderGetApplyGet)
{
    auto fixture_text = ReadFixtureText("post84_accepted_properties.json");
    ASSERT_TRUE(fixture_text.has_value()) << "fixture missing";
    auto interface_path =
        FixturePath("interface_authoring_v28.jsonc").generic_string();

    // 把字面量 fixture 装进 session context。interfacePath 用 v28 fixture
    // 的绝对路径覆盖（fixture 内的相对值只是占位）。
    auto parsed = ParseJsonFromStringStrict(*fixture_text);
    ASSERT_TRUE(parsed.has_value());
    auto root_obj = parsed->as_object();
    ASSERT_TRUE(root_obj.has_value());
    auto pi_obj = (*root_obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi_obj.has_value());

    std::ostringstream context_buf;
    context_buf << "{\"adapter\":{\"interfacePath\":\"" << interface_path
                << "\"},\"pi\":";
    context_buf << SerializeOrEmpty((*root_obj)[std::string_view("pi")]);
    context_buf << "}";
    auto session = CreateSession(context_buf.str());

    // Get #1: 必须看见 cfg_a 的两个 DailyFarm item。
    DasPtr<Das::ExportInterface::IDasJson> get_json;
    ASSERT_EQ(session->GetDocument({}, get_json.Put()), DAS_S_OK);
    // Production-path: Provider GetDocument returns the envelope directly.
    auto envelope1 = ReadJsonInterface(get_json.Get());
    auto get_valid1 =
        Das::Core::TaskScheduler::ValidateAuthoringGetResult(envelope1);
    EXPECT_TRUE(get_valid1.valid) << "error_kind=" << get_valid1.error_kind
                                  << " message=" << get_valid1.message;

    auto document1 = DocumentOfGet(get_json.Get());

    auto ids1 = CollectFormItemIds(document1);
    ASSERT_EQ(ids1.size(), 2u);
    EXPECT_EQ(ids1[0], "item_daily_farm_a1");
    EXPECT_EQ(ids1[1], "item_daily_farm_a2");

    // Apply #1: 关闭 item_daily_farm_a2 的 enabled。
    DasPtr<Das::ExportInterface::IDasJson> apply_json;
    auto                                   change1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
        "\"target\":{\"kind\":\"formSequenceItem\",\"id\":\"item_daily_farm_"
        "a2\"},"
        "\"fieldName\":\"enabled\",\"value\":false}}");
    ASSERT_EQ(session->ApplyChange(change1.Get(), apply_json.Put()), DAS_S_OK);
    auto apply1 = ReadJsonInterface(apply_json.Get());
    auto apply_valid1 =
        Das::Core::TaskScheduler::ValidateAuthoringApplyResult(apply1, 0);
    EXPECT_TRUE(apply_valid1.valid) << "error_kind=" << apply_valid1.error_kind
                                    << " message=" << apply_valid1.message;
    EXPECT_EQ(DocumentRevisionFromApplyResult(apply1), 1);

    auto apply1_doc = DocumentFromApplyResult(apply1);
    auto ids_after_apply1 = CollectFormItemIds(apply1_doc);
    ASSERT_EQ(ids_after_apply1.size(), 2u);
    EXPECT_EQ(ids_after_apply1[0], "item_daily_farm_a1");
    EXPECT_EQ(ids_after_apply1[1], "item_daily_farm_a2");

    // Apply #2: D-84.2-03 presetName setValue（D-84-41 保留路径）。
    // configurations 已删除（D-84.2-01），改用 presetName 切换触发
    // ApplyPreset。
    DasPtr<Das::ExportInterface::IDasJson> apply2_json;
    auto                                   change2 = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"setValue\",\"payload\":{"
        "\"target\":{\"kind\":\"document\"},\"fieldName\":\"presetName\","
        "\"value\":\"DailyPreset\"}}");
    ASSERT_EQ(session->ApplyChange(change2.Get(), apply2_json.Put()), DAS_S_OK);
    auto apply2 = ReadJsonInterface(apply2_json.Get());
    auto apply_valid2 =
        Das::Core::TaskScheduler::ValidateAuthoringApplyResult(apply2, 1);
    EXPECT_TRUE(apply_valid2.valid) << "error_kind=" << apply_valid2.error_kind
                                    << " message=" << apply_valid2.message;
    EXPECT_EQ(DocumentRevisionFromApplyResult(apply2), 2);

    // 新 session 从同一字面量重启（模拟 reload）。Get 应该和首次 Get 一致，
    // 证明 Get 投影稳定，不依赖 session 之间的内部状态。
    auto session2 = CreateSession(context_buf.str());
    DasPtr<Das::ExportInterface::IDasJson> get2_json;
    ASSERT_EQ(session2->GetDocument({}, get2_json.Put()), DAS_S_OK);
    auto document2 = DocumentOfGet(get2_json.Get());
    auto ids2 = CollectFormItemIds(document2);
    ASSERT_EQ(ids2.size(), 2u);
    EXPECT_EQ(ids2[0], "item_daily_farm_a1");
    EXPECT_EQ(ids2[1], "item_daily_farm_a2");
}

// Tracer 2 RED: 字面量 post-84 state 支持完整编辑生命周期，且每个阶段都
// 通过 84-01 validator。覆盖：duplicate actionId add、setValue/enabled、
// move、remove、Compile。（D-84.2-02 已删除 newConfiguration/copyConfiguration
// 命令；newPreset 入口由 InvokeCommandNewPresetCreatesSnapshot 覆盖。）
TEST_F(MaapiAuthoringFixture, LiteralPost84StateFullLifecyclePreservesAuthority)
{
    auto fixture_text = ReadFixtureText("post84_accepted_properties.json");
    ASSERT_TRUE(fixture_text.has_value()) << "fixture missing";
    auto interface_path =
        FixturePath("interface_authoring_v28.jsonc").generic_string();

    std::ostringstream context_buf;
    context_buf << "{\"adapter\":{\"interfacePath\":\"" << interface_path
                << "\"},\"pi\":";
    auto parsed = ParseJsonFromStringStrict(*fixture_text);
    ASSERT_TRUE(parsed.has_value());
    auto root_obj = parsed->as_object();
    ASSERT_TRUE(root_obj.has_value());
    context_buf << SerializeOrEmpty((*root_obj)[std::string_view("pi")]);
    context_buf << "}";
    auto session = CreateSession(context_buf.str());

    int64_t expected_revision = 0;

    auto expect_apply_ok = [&](Das::ExportInterface::IDasJson*  change,
                               Das::ExportInterface::IDasJson** out_result)
    {
        DasPtr<Das::ExportInterface::IDasJson> result_json;
        ASSERT_EQ(session->ApplyChange(change, result_json.Put()), DAS_S_OK);
        auto result = ReadJsonInterface(result_json.Get());
        auto base = expected_revision;
        auto v = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
            result,
            base);
        EXPECT_TRUE(v.valid)
            << "error_kind=" << v.error_kind << " message=" << v.message;
        EXPECT_EQ(DocumentRevisionFromApplyResult(result), base + 1);
        EXPECT_EQ(CountDiagnosticsBySeverity(result, "error"), 0u);
        ++expected_revision;
        if (out_result)
        {
            *out_result = result_json.Get();
            if (*out_result)
            {
                (*out_result)->AddRef();
            }
        }
    };

    // 1) Duplicate actionId add：再添加一个 DailyFarm。
    {
        auto change = ParseDasJson(
            "{\"baseRevision\":0,\"kind\":\"addFormSequenceItem\",\"payload\":{"
            "\"actionId\":\"DailyFarm\"}}");
        expect_apply_ok(change.Get(), nullptr);
    }

    // 2) setValue formSequenceItem enabled toggle。
    {
        auto change = ParseDasJson(
            "{\"baseRevision\":1,\"kind\":\"setValue\",\"payload\":{"
            "\"target\":{\"kind\":\"formSequenceItem\",\"id\":\"item_daily_farm_"
            "a1\"},"
            "\"fieldName\":\"enabled\",\"value\":false}}");
        expect_apply_ok(change.Get(), nullptr);
    }

    // 3) moveFormSequenceItem: 0 → 1.
    {
        auto change = ParseDasJson(
            "{\"baseRevision\":2,\"kind\":\"moveFormSequenceItem\",\"payload\":{"
            "\"formSequenceItemId\":\"item_daily_farm_a1\","
            "\"destinationIndex\":1}}");
        expect_apply_ok(change.Get(), nullptr);
    }

    // 4) removeFormSequenceItem.
    {
        auto change = ParseDasJson(
            "{\"baseRevision\":3,\"kind\":\"removeFormSequenceItem\","
            "\"payload\":{\"formSequenceItemId\":\"item_daily_farm_a2\"}}");
        expect_apply_ok(change.Get(), nullptr);
    }

    // 5) Compile：严格 envelope 通过 validator。
    {
        DasPtr<Das::ExportInterface::IDasJson> compile_json;
        ASSERT_EQ(session->Compile({}, compile_json.Put()), DAS_S_OK);
        auto compile = ReadJsonInterface(compile_json.Get());
        auto v =
            Das::Core::TaskScheduler::ValidateAuthoringCompileResult(compile);
        EXPECT_TRUE(v.valid)
            << "error_kind=" << v.error_kind << " message=" << v.message;
        auto obj = compile.as_object();
        ASSERT_TRUE(obj.has_value());
        EXPECT_TRUE(obj->contains(std::string_view("compile")));
        EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));
    }
}

// Tracer 3 RED: malformed Provider Apply result 留下 disposable 持久状态不变。
// 通过 public session seam 发送 legacy valuePath payload（84-07 拒绝为
// unsupportedChange），断言 revision/acceptedProperties 不变。
TEST_F(MaapiAuthoringFixture, MalformedChangeLeavesDisposableStateUnchanged)
{
    auto fixture_text = ReadFixtureText("post84_accepted_properties.json");
    ASSERT_TRUE(fixture_text.has_value()) << "fixture missing";
    auto interface_path =
        FixturePath("interface_authoring_v28.jsonc").generic_string();
    auto parsed = ParseJsonFromStringStrict(*fixture_text);
    ASSERT_TRUE(parsed.has_value());
    auto root_obj = parsed->as_object();
    ASSERT_TRUE(root_obj.has_value());

    std::ostringstream context_buf;
    context_buf << "{\"adapter\":{\"interfacePath\":\"" << interface_path
                << "\"},\"pi\":";
    context_buf << SerializeOrEmpty((*root_obj)[std::string_view("pi")]);
    context_buf << "}";
    auto session = CreateSession(context_buf.str());

    // 记录初始 acceptedProperties 的 normalized 文本，作为 disposable state
    // baseline。每个 Apply 失败之后必须等于这个 baseline。
    DasPtr<Das::ExportInterface::IDasJson> get_json;
    ASSERT_EQ(session->GetDocument({}, get_json.Put()), DAS_S_OK);
    auto initial_doc = DocumentOfGet(get_json.Get());
    EXPECT_EQ(DocumentRevision(initial_doc), 0);

    // 失败 #1: legacy valuePath payload → 84-07 unsupportedChange。
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_json;
        auto                                   change = ParseDasJson(
            "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
            "\"valuePath\":\"controllerName\",\"value\":\"Desktop\"}}");
        ASSERT_EQ(
            session->ApplyChange(change.Get(), apply_json.Put()),
            DAS_S_OK);
        auto result = ReadJsonInterface(apply_json.Get());
        // 严格 Apply envelope 仍要满足 84-01 schema（即便携带 error 诊断）。
        // document.revision 必须 == baseRevision（语义失败不递增）。
        EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);
        EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    }

    // 失败 #2: legacy applyPreset kind → unsupportedChange。
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_json;
        auto                                   change = ParseDasJson(
            "{\"baseRevision\":0,\"kind\":\"applyPreset\",\"payload\":{"
            "\"presetName\":\"DailyPreset\"}}");
        ASSERT_EQ(
            session->ApplyChange(change.Get(), apply_json.Put()),
            DAS_S_OK);
        auto result = ReadJsonInterface(apply_json.Get());
        EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);
        EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    }

    // 失败 #3: baseRevision mismatch → 不递增、不写入。
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_json;
        auto                                   change = ParseDasJson(
            "{\"baseRevision\":42,\"kind\":\"setValue\",\"payload\":{"
            "\"target\":{\"kind\":\"document\"},\"fieldName\":\"controllerName\","
            "\"value\":\"Desktop\"}}");
        ASSERT_EQ(
            session->ApplyChange(change.Get(), apply_json.Put()),
            DAS_S_OK);
        auto result = ReadJsonInterface(apply_json.Get());
        EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);
        EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    }

    // 重新 Get：document 仍是初始状态，revision 仍是 0。
    DasPtr<Das::ExportInterface::IDasJson> post_get_json;
    ASSERT_EQ(session->GetDocument({}, post_get_json.Put()), DAS_S_OK);
    auto post_doc = DocumentOfGet(post_get_json.Get());
    EXPECT_EQ(DocumentRevision(post_doc), 0);
    auto post_ids = CollectFormItemIds(post_doc);
    ASSERT_EQ(post_ids.size(), 2u);
    EXPECT_EQ(post_ids[0], "item_daily_farm_a1");
    EXPECT_EQ(post_ids[1], "item_daily_farm_a2");
}

// Tracer 4 RED: legacy taskId7 只读边界。pre/post SHA-256 必须相等，
// 且 session 读取 legacy（无 itemId、含 authoring.kind/migration）的内容时
// Provider 不会崩溃：document 可以生产（NormalizeItemIds 在 parse 时一次性
// 分配稳定 itemId），acceptedProperties 不会回写到磁盘文件。
//
// gate 是 SHA-256 hash 相等，不是 `git diff --exit-code`（gitignored 路径
// 永远 report no diff）。
TEST_F(MaapiAuthoringFixture, LegacyTaskId7BoundaryPreservedReadOnlyWithSha256)
{
    const auto legacy_path = LegacyTaskId7Path();
    const auto pre_bytes_opt = ReadFileBytes(legacy_path);
    if (!pre_bytes_opt.has_value())
    {
        GTEST_SKIP() << "taskId7.json not present at " << legacy_path
                     << " (CI / fresh clone); skip legacy boundary test";
    }
    const auto& pre_bytes = *pre_bytes_opt;
    const auto  pre_hash = Sha256(pre_bytes).Hex();

    // 把 legacy content 摊平成 context。properties.pi → context.pi；
    // adapter.interfacePath 重定向到 v28 fixture（原路径是用户的真实路径）。
    auto parsed = ParseJsonFromStringStrict(pre_bytes);
    ASSERT_TRUE(parsed.has_value()) << "taskId7.json is not valid JSON";
    auto root_obj = parsed->as_object();
    ASSERT_TRUE(root_obj.has_value());
    ASSERT_TRUE(root_obj->contains(std::string_view("properties")));
    auto props = (*root_obj)[std::string_view("properties")].as_object();
    ASSERT_TRUE(props.has_value());
    ASSERT_TRUE(props->contains(std::string_view("pi")));

    auto interface_path =
        FixturePath("interface_authoring_v28.jsonc").generic_string();
    std::ostringstream context_buf;
    context_buf << "{\"adapter\":{\"interfacePath\":\"" << interface_path
                << "\"},\"pi\":";
    context_buf << SerializeOrEmpty((*props)[std::string_view("pi")]);
    context_buf << "}";
    auto session = CreateSession(context_buf.str());

    // Get：document 必须能生产；legacy 缺 itemId 由 NormalizeItemIds 分配。
    DasPtr<Das::ExportInterface::IDasJson> get_json;
    ASSERT_EQ(session->GetDocument({}, get_json.Put()), DAS_S_OK);
    // Production-path: Provider GetDocument returns the envelope directly.
    auto envelope = ReadJsonInterface(get_json.Get());
    auto get_valid =
        Das::Core::TaskScheduler::ValidateAuthoringGetResult(envelope);
    EXPECT_TRUE(get_valid.valid) << "error_kind=" << get_valid.error_kind
                                 << " message=" << get_valid.message;
    auto document = DocumentOfGet(get_json.Get());

    // formSequence.items[] 必须全部有非空 id（legacy 没 itemId，
    // NormalizeItemIds 必须补齐）。
    auto item_ids = CollectFormItemIds(document);
    ASSERT_FALSE(item_ids.empty());
    for (const auto& id : item_ids)
    {
        EXPECT_FALSE(id.empty());
    }

    // 重新读 taskId7.json 的字节，SHA-256 必须与 pre 完全一致。
    // gate = SHA-256 hash 相等（不是 git diff --exit-code）。
    const auto post_bytes_opt = ReadFileBytes(legacy_path);
    ASSERT_TRUE(post_bytes_opt.has_value());
    const auto& post_bytes = *post_bytes_opt;
    const auto  post_hash = Sha256(post_bytes).Hex();
    EXPECT_EQ(pre_hash, post_hash)
        << "taskId7.json mutated by Phase 84-08 test; pre=" << pre_hash
        << " post=" << post_hash;
    EXPECT_EQ(pre_bytes, post_bytes) << "taskId7.json byte content changed";
}

// Tracer 4 RED (DTO 级): 字面量 legacy pre-84 pi（taskId7 的形态）→
// ParseAcceptedSettings 不崩溃；acceptedProperties 持久化 schema 不含
// authoring.kind / authoring.migration / acceptedProperties.migration。
TEST(MaapiLegacyBoundaryTest, LegacyPre84ParseDoesNotEmitAuthoringLegacyFields)
{
    // 字面量构造 pre-84 形态（与 taskId7.json 同结构），不读 live 文件本身。
    // ParseAcceptedSettings 期望 acceptedProperties envelope shape（含 `pi`
    // 顶层字段），与 taskId7.json 的 properties.pi 形态一致。
    static constexpr std::string_view kLegacyAccepted = R"({
      "version": 1,
      "adapter": {"interfacePath": "D:/softwares/green/MaaEnd-win/interface.json"},
      "pi": {
        "controllerName": "Win32-Front",
        "resourceName": "Official",
        "presetName": "QuickDaily",
        "language": "zh_cn",
        "globalOptions": [],
        "resourceOptions": [],
        "controllerOptions": [],
        "tasks": [
          {"taskName": "VisitFriends", "enabled": true, "options": []},
          {"taskName": "VisitFriends", "enabled": false, "options": []}
        ],
        "orphanPaths": []
      }
    })";
    auto parsed = Das::Utils::ParseYyjsonFromString(kLegacyAccepted);
    ASSERT_TRUE(parsed.has_value());
    auto settings = Das::Plugins::DasMaaPi::ParseAcceptedSettings(*parsed);

    // 两个同名 VisitFriends task 必须拿到不同的 itemId (NormalizeItemIds 在
    // parse 时一次性分配稳定 itemId，确保 Get 投影幂等)。
    ASSERT_EQ(settings.pi.tasks.size(), 2u);
    EXPECT_FALSE(settings.pi.tasks[0].item_id.empty());
    EXPECT_FALSE(settings.pi.tasks[1].item_id.empty());
    EXPECT_NE(settings.pi.tasks[0].item_id, settings.pi.tasks[1].item_id);

    // 重新序列化后的 acceptedProperties 不应携带 migration 字段。
    auto serialized =
        Das::Plugins::DasMaaPi::SerializeAcceptedSettings(settings);
    auto serialized_obj = serialized.as_object();
    ASSERT_TRUE(serialized_obj.has_value());
    EXPECT_FALSE(serialized_obj->contains(std::string_view("migration")));
    EXPECT_FALSE(
        serialized_obj->contains(std::string_view("sourceFingerprint")));
}

// ---------------------------------------------------------------------------
// UAT 回归：PI 接口定义的默认静态设置必须投影到 Authoring doc 的 fields
//
// 背景：用户在 UI 创建 MaaPI 任务后，GetDocument 返回的 document.fields
// 缺失 PI 接口定义的 default 值。根因是 ProjectAuthoringDocument 投影
// catalog.global_options 时，acceptedProperties.pi.global_options 在 fresh
// session 下为空 (current=nullptr)，MakeOptionPropertyField 对 select /
// checkbox / switch 类型未回退到 PI 接口定义的 default_case / 首个 case /
// switch 默认 true 约定，导致 FieldDescriptor.value 始终为空字符串/空数组/
// false。同理，task 级 option 当 acceptedProperties 外部加载且 task.options
// 缺省时也走同一 nullptr 路径。
//
// 这些测试用 MakeStrictProjectionMockCatalog (没有 literal fixture 的
// 完整 acceptedProperties)，强制 current=nullptr，确保投影端独立满足
// "PI 接口默认值进入 fields" 的契约。
// ---------------------------------------------------------------------------

namespace
{
    yyjson::value FieldValueOf(
        const yyjson::value& document,
        std::string_view     name)
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
                == name)
            {
                return Das::Utils::CloneYyjsonValue(
                    (*field)[std::string_view("value")]);
            }
        }
        return yyjson::value(nullptr);
    }

    // 构造一个覆盖 select(input)/checkbox/switch 四种 option 类型，且每种都
    // 携带 PI 协议层面 default 信号的 mock catalog。stage 与 settings 是
    // global_options，notify 是 task 级 switch。stage 没有 default_case，
    // cases=[1-1,1-2]；explore 有 default_case=[hard]；notify 是 switch。
    PiCatalog MakePiDefaultProjectionMockCatalog()
    {
        using namespace Das::Plugins::DasMaaPi;
        PiCatalog catalog;
        catalog.name = "PiDefaultProjection";
        catalog.version = "1.0";

        PiTask task;
        task.dto.name = "DailyFarm";
        task.dto.entry = "StartDaily";
        task.dto.option = {"stage", "explore", "notify"};
        catalog.tasks.push_back(std::move(task));

        auto make_select_stage = []() -> PiOption
        {
            PiOption o;
            o.dto.name = "stage";
            o.dto.type = "select";
            PiCaseDto c1;
            c1.name = "1-1";
            PiCaseDto c2;
            c2.name = "1-2";
            o.dto.cases.push_back(std::move(c1));
            o.dto.cases.push_back(std::move(c2));
            // 没有 default_case —— PI 默认应回退到首个 case "1-1"。
            return o;
        };

        auto make_select_explore = []() -> PiOption
        {
            PiOption o;
            o.dto.name = "explore";
            o.dto.type = "select";
            PiCaseDto easy;
            easy.name = "easy";
            PiCaseDto hard;
            hard.name = "hard";
            o.dto.cases.push_back(std::move(easy));
            o.dto.cases.push_back(std::move(hard));
            o.dto.default_cases.push_back("hard");
            return o;
        };

        auto make_switch = []() -> PiOption
        {
            PiOption o;
            o.dto.name = "notify";
            o.dto.type = "switch";
            return o;
        };

        catalog.options.push_back(make_select_stage());
        catalog.options.push_back(make_select_explore());
        catalog.options.push_back(make_switch());

        // stage 与 explore 都注册为 global_option（既出现在 catalog.options
        // 也出现在 catalog.global_options），模拟 PI interface.json 的
        // global_option 字段。notify 仅 task 级。
        for (const auto& opt : catalog.options)
        {
            if (opt.dto.name == "stage" || opt.dto.name == "explore")
            {
                catalog.global_options.push_back(opt);
            }
        }
        return catalog;
    }
} // namespace

// UAT-RED: fresh session (空 acceptedProperties.pi.global_options) →
// document.fields[options.stage].value 必须是 PI 首个 case "1-1"
// (select 无 default_case 时回退首个 case)；options.explore.value 必须
// 是 PI default_case "hard"。修复前 current=nullptr 导致返回空字符串。
TEST(MaapiProjectionTest, GlobalOptionsProjectPiInterfaceDefaultsWhenUnset)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakePiDefaultProjectionMockCatalog();

    // fresh session：没有任何用户运行时设置。
    AcceptedSettingsDto settings;

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    // 严格 contract 仍要通过。
    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    // stage: select 无 default_case → 回退首个 case "1-1"。
    auto stage_val = FieldValueOf(document, "options.stage");
    ASSERT_TRUE(stage_val.is_string())
        << "options.stage value must be string (select scalar IR)";
    EXPECT_EQ(stage_val.as_string().value_or(""), "1-1");

    // explore: select 有 default_case=["hard"] → 使用 "hard"。
    auto explore_val = FieldValueOf(document, "options.explore");
    ASSERT_TRUE(explore_val.is_string())
        << "options.explore value must be string (select scalar IR)";
    EXPECT_EQ(explore_val.as_string().value_or(""), "hard");
}

// UAT-RED: 用户已通过 setValue 把 stage 改为 "1-2"，则 projection 必须呈现
// 用户值而不是 PI 默认。这保证修复不会覆盖用户运行时值。
TEST(MaapiProjectionTest, GlobalOptionsRespectUserValueOverPiDefault)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakePiDefaultProjectionMockCatalog();

    AcceptedSettingsDto      settings;
    MaapiPiOptionSettingsDto user_stage;
    user_stage.option_name = "stage";
    user_stage.kind = "select";
    user_stage.selected_cases.push_back("1-2");
    settings.pi.global_options.push_back(std::move(user_stage));

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto stage_val = FieldValueOf(document, "options.stage");
    ASSERT_TRUE(stage_val.is_string());
    EXPECT_EQ(stage_val.as_string().value_or(""), "1-2");

    // explore 用户没改 → 仍走 PI default_case "hard"。
    auto explore_val = FieldValueOf(document, "options.explore");
    ASSERT_TRUE(explore_val.is_string());
    EXPECT_EQ(explore_val.as_string().value_or(""), "hard");
}

// UAT-RED: task 级 switch option 在 acceptedProperties.tasks[i].options 缺省
// 时（外部加载 acceptedProperties 没有该 option），projection 必须呈现 PI
// 默认 true（MaaPI 约定，见 InitializeOptionDefaults）。
TEST(MaapiProjectionTest, TaskSwitchOptionFallsBackToPiDefaultWhenMissing)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakePiDefaultProjectionMockCatalog();

    AcceptedSettingsDto    settings;
    MaapiPiTaskSettingsDto task;
    task.item_id = "item_daily_farm";
    task.task_name = "DailyFarm";
    task.enabled = true;
    // 故意不填 task.options：模拟外部加载的 acceptedProperties
    // (post84 fixture 的 item_daily_farm_a2 形态：只配置了 stage)。
    settings.pi.tasks.push_back(std::move(task));

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    // notify: switch 缺省 → PI 默认 true。
    auto notify_val = FindSequenceItemFieldValue(
        document,
        "item_daily_farm",
        "options.notify");
    ASSERT_TRUE(notify_val.is_bool());
    EXPECT_TRUE(notify_val.as_bool().value_or(false));

    // stage: select 缺省 → 回退首个 case "1-1"。
    auto stage_val = FindSequenceItemFieldValue(
        document,
        "item_daily_farm",
        "options.stage");
    ASSERT_TRUE(stage_val.is_string());
    EXPECT_EQ(stage_val.as_string().value_or(""), "1-1");
}

// ---------------------------------------------------------------------------
// itemId 偶现 bug 回归 (FormSequenceItem.id must be non-empty)
//
// 背景：前端 DasNativeUI 编辑 MaaPI formSequence 任务时偶发
// "FormSequenceItem.id must be non-empty"。根因是若干 apply 路径在
// settings.pi.tasks 里引入了空 item_id 的 task 实例：
//   1) ApplyPreset 清空 tasks 后按 preset 重建，重建过程不分配 itemId。
//   2) FindTaskOptionContainer（task 级 option 的 document-targeted setValue）
//      找不到现有 task 时按 catalog 注册新建一个，不分配 itemId。
// TaskSettingsArray 序列化对空 item_id 跳过，持久化丢失；ParseAcceptedSettings
// 仅在 parse 时兜底（session long-lived，apply 后不 normalize），故偶现。
//
// 这些测试构造可复现路径，断言 Get/Apply 投影后 formSequence.items[*].id 全部
// 非空且唯一。修复前为 RED。
// ---------------------------------------------------------------------------

// 断言 document.formSequence.items[*].id 全部非空、唯一。
::testing::AssertionResult AllItemIdsNonEmptyAndUnique(
    const yyjson::value& document)
{
    auto ids = CollectFormItemIds(document);
    if (ids.empty())
    {
        return ::testing::AssertionFailure()
               << "no formSequence items found in document";
    }
    std::set<std::string> seen;
    for (const auto& id : ids)
    {
        if (id.empty())
        {
            return ::testing::AssertionFailure()
                   << "formSequence item has empty id";
        }
        if (!seen.insert(id).second)
        {
            return ::testing::AssertionFailure()
                   << "duplicate formSequence item id: " << id;
        }
    }
    return ::testing::AssertionSuccess();
}

// RED-1: ApplyPreset 经 session ApplyChange 触发，重建 tasks 必须给每个实例
// 分配稳定 itemId。修复前 ApplyPreset 不分配，document.formSequence.items[0].id
// 为空，frontend 报 "FormSequenceItem.id must be non-empty"。
TEST_F(MaapiAuthoringFixture, PresetExpansionAssignsStableItemIds)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   preset_change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"presetName\","
        "\"value\":\"DailyPreset\"}}");
    ASSERT_EQ(
        session->ApplyChange(preset_change.Get(), result_json.Put()),
        DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto apply_valid =
        Das::Core::TaskScheduler::ValidateAuthoringApplyResult(result, 0);
    EXPECT_TRUE(apply_valid.valid) << "error_kind=" << apply_valid.error_kind
                                   << " message=" << apply_valid.message;

    auto document = DocumentFromApplyResult(result);
    EXPECT_TRUE(AllItemIdsNonEmptyAndUnique(document));

    // Get 再次投影：id 必须稳定（与 Apply 结果一致，幂等）。
    DasPtr<Das::ExportInterface::IDasJson> get_json;
    ASSERT_EQ(session->GetDocument({}, get_json.Put()), DAS_S_OK);
    auto get_document = DocumentOfGet(get_json.Get());
    EXPECT_TRUE(AllItemIdsNonEmptyAndUnique(get_document));

    auto apply_ids = CollectFormItemIds(document);
    auto get_ids = CollectFormItemIds(get_document);
    ASSERT_EQ(apply_ids.size(), get_ids.size());
    for (std::size_t i = 0; i < apply_ids.size(); ++i)
    {
        EXPECT_EQ(apply_ids[i], get_ids[i])
            << "item id unstable between Apply and Get at index " << i;
    }
}

// RED-2: setValue target=document 对 task 级 option（notify 不在 global_option
// 列表，是 DailyFarm 私有）触发 FindTaskOptionContainer 新建 task 实例。
// 修复前该路径不分配 itemId，doc.item.id 为空。
TEST_F(
    MaapiAuthoringFixture,
    DocumentTargetSetValueOnTaskScopedOptionAssignsItemId)
{
    auto interface_path =
        FixturePath("interface_authoring.jsonc").generic_string();
    auto session = CreateSession(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    // notify 是 DailyFarm 的 task 级 switch option；target=document 触发
    // ApplyOptionValueChange → FindTaskOptionContainer 新建 DailyFarm 实例。
    auto change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"options.notify\","
        "\"value\":true}}");
    ASSERT_EQ(session->ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);

    auto result = ReadJsonInterface(result_json.Get());
    auto document = DocumentFromApplyResult(result);
    EXPECT_TRUE(AllItemIdsNonEmptyAndUnique(document));

    auto ids = CollectFormItemIds(document);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_FALSE(ids[0].empty());
}

// RED-3: 单元级 —— 直接构造带空 item_id 的 settings，投影必须保证
// formSequence.items[*].id 非空（投影层兜底）。修复前投影原样输出空字符串。
TEST(MaapiItemIdSafetyNetTest, ProjectAuthoringDocumentFillsEmptyItemId)
{
    using namespace Das::Plugins::DasMaaPi;
    PiCatalog catalog;
    catalog.name = "ItemIdSafetyNet";
    catalog.version = "1.0";

    PiTask task;
    task.dto.name = "DailyFarm";
    task.dto.entry = "StartDaily";
    catalog.tasks.push_back(std::move(task));

    AcceptedSettingsDto    settings;
    MaapiPiTaskSettingsDto t1;
    t1.task_name = "DailyFarm";
    t1.enabled = true;
    // 故意留空 item_id —— 模拟漏分配的 apply 路径产生的脏状态。
    settings.pi.tasks.push_back(std::move(t1));

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    EXPECT_TRUE(AllItemIdsNonEmptyAndUnique(document));

    // 第二次投影同一 settings：id 应保持稳定（Layer 2 已在 apply 层补 id；
    // 单元测试直接验证 Layer 1 投影兜底也能给出非空 id）。
    auto document_again = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    EXPECT_TRUE(AllItemIdsNonEmptyAndUnique(document_again));
}
