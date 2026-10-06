#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/TaskAuthoringContract.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/AuthoringProjector.h>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/Plugins/DasMaaPi/PiCompiler.h>
#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressService.Implements.hpp>
#include <gtest/gtest.h>
#include <zlib.h>

// D-89-03: 会话级直接测试（不经 IPC）——MaapiAuthoringSession 是 final 具体类，
// DasMaaPiTest 直接编译本类 + PiUpdateService + FakeHttpClient seam，可无 IPC
// 验证 checkUpdate/applyUpdate 全链路 + 检查结果表（进程级 store）。
#include "MaapiAuthoringSession.h"
#include "PiUpdateService.h"
#include "UpdateCheckStore.h"
#include "UpdateJobRegistry.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <atomic>

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
    // 校验。command_result 可选业务结果回传（checkUpdate → progressKey）。
    // 更新五字段与 applyUpdate 按钮态由进程级 UpdateCheckStore 条目驱动。
    // device_cache 供 adbDevice 字段投影 candidates。component_context 同
    // BuildDocument 语境门控语义（缺省 false = standalone）。
    yyjson::value BuildApplyResult(
        const AcceptedSettingsDto&        settings,
        int64_t                           revision,
        std::vector<PiDiagnosticDto>      diagnostics,
        bool                              project_document = true,
        const yyjson::value*              command_result = nullptr,
        const std::vector<AdbDeviceInfo>* device_cache = nullptr,
        bool                              component_context = false);
} // namespace Das::Plugins::DasMaaPi

namespace
{
    using namespace Das;
    using namespace Das::Core::ForeignInterfaceHost;
    using namespace Das::Plugins::DasMaaPi;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY;

    std::filesystem::path PluginPackageDir()
    {
        return IpcTestConfig::GetPluginDir() / DAS_PATH_LITERAL("DasMaaPi");
    }

    std::filesystem::path MaapiManifestPath()
    {
        return PluginPackageDir() / DAS_PATH_LITERAL("DasMaaPi.json");
    }

    std::filesystem::path FixturePath(std::string_view name)
    {
        return std::filesystem::current_path() / DAS_PATH_LITERAL("DasMaaPi")
               / DAS_PATH_LITERAL("test") / DAS_PATH_LITERAL("fixtures")
               / Das::Utils::PathFromUtf8(name);
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
                keys.emplace(key);
                CollectKeysRecursive(val, keys);
            }
            return;
        }
        if (auto arr = value.as_array())
        {
            for (auto&& it : *arr)
            {
                CollectKeysRecursive(it, keys);
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
               / Das::Utils::PathFromUtf8(
                   "das_maapi_authoring_" + std::to_string(ticks));
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
            ipc_sp_ = Das::Core::IPC::MainProcess::CreateIpcContext(false);
            ipc_run_thread_ = std::thread([this]() { ipc_sp_->Run(); });
            plugin_manager_ = std::make_unique<PluginManager>(
                *settings_manager_,
                Das::DasRef<Das::Core::IPC::MainProcess::IIpcContext>(
                    ipc_sp_.Get()));
            ASSERT_EQ(plugin_manager_->Initialize(1), DAS_S_OK);
            // 模拟生产 DasHttp 启动设 host path（App.cpp:146），让 loadMode:ipc
            // 的 DasMaaPi.json 能 spawn 真实 DasHost。host
            // 未配置/不存在则跳过。
            std::filesystem::path das_host_path;
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
                GTEST_SKIP() << "DasHost not found at: "
                             << Das::Utils::PathToUtf8(das_host_path);
            }
            // ABI 边界收 const std::string&，path 在调用点物化为 UTF-8
            plugin_manager_->SetHostExePath(
                Das::Utils::PathToUtf8(das_host_path));
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
                    MakeDasGuid(std::string(TASK_GUID_TEXT)),
                    context_json.Get(),
                    session.Put()),
                DAS_S_OK);
            return session;
        }

        std::filesystem::path settings_dir_;
        std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
        std::unique_ptr<PluginManager>                        plugin_manager_;
        Das::DasPtr<Das::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
        std::thread                                           ipc_run_thread_;
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

    // 在 document.fieldLayout[] 中按 section.id 查找 section。
    std::optional<yyjson::value> FindLayoutSection(
        const yyjson::value& document,
        std::string_view     id)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("fieldLayout")))
        {
            return std::nullopt;
        }
        auto layout = (*obj)[std::string_view("fieldLayout")].as_array();
        if (!layout)
        {
            return std::nullopt;
        }
        for (auto it = layout->begin(); it != layout->end(); ++it)
        {
            auto section = it->as_object();
            if (!section)
            {
                continue;
            }
            if ((*section)[std::string_view("id")].as_string().value_or("")
                == id)
            {
                return Das::Utils::CloneYyjsonValue(*it);
            }
        }
        return std::nullopt;
    }

    // 收集 section.entries[] 所有叶子 fieldName（保持声明顺序）。跳过非
    // fieldName 叶子（{commandId} 叶子、{row:[...]} 容器等）— cpp_yyjson
    // operator[] 对缺失 key 抛异常，必须先 contains 检查。
    std::vector<std::string> LayoutSectionFieldNames(
        const yyjson::value& section)
    {
        std::vector<std::string> names;
        auto                     section_obj = section.as_object();
        if (!section_obj || !section_obj->contains(std::string_view("entries")))
        {
            return names;
        }
        auto entries = (*section_obj)[std::string_view("entries")].as_array();
        if (!entries)
        {
            return names;
        }
        for (auto it = entries->begin(); it != entries->end(); ++it)
        {
            auto leaf = it->as_object();
            if (!leaf || !leaf->contains(std::string_view("fieldName")))
            {
                continue;
            }
            names.emplace_back(
                (*leaf)[std::string_view("fieldName")].as_string().value_or(
                    ""));
        }
        return names;
    }

    // 收集 document.fields[] 所有 name（保持声明顺序）。
    std::vector<std::string> RootFieldNames(const yyjson::value& document)
    {
        std::vector<std::string> names;
        auto                     obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("fields")))
        {
            return names;
        }
        auto fields = (*obj)[std::string_view("fields")].as_array();
        if (!fields)
        {
            return names;
        }
        for (auto it = fields->begin(); it != fields->end(); ++it)
        {
            auto field = it->as_object();
            if (!field)
            {
                continue;
            }
            names.emplace_back(
                (*field)[std::string_view("name")].as_string().value_or(""));
        }
        return names;
    }

    // 遍历 document.fieldLayout[] 各 section.entries[]，收集所有叶子
    // fieldName。跳过非 fieldName 叶子（{commandId} 叶子、{row:[...]} 容器）。
    std::vector<std::string> LayoutLeafFieldNames(const yyjson::value& document)
    {
        std::vector<std::string> names;
        auto                     obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("fieldLayout")))
        {
            return names;
        }
        auto layout = (*obj)[std::string_view("fieldLayout")].as_array();
        if (!layout)
        {
            return names;
        }
        for (auto it = layout->begin(); it != layout->end(); ++it)
        {
            auto section = it->as_object();
            if (!section || !section->contains(std::string_view("entries")))
            {
                continue;
            }
            auto entries = (*section)[std::string_view("entries")].as_array();
            if (!entries)
            {
                continue;
            }
            for (auto eit = entries->begin(); eit != entries->end(); ++eit)
            {
                auto leaf = eit->as_object();
                if (!leaf || !leaf->contains(std::string_view("fieldName")))
                {
                    continue;
                }
                names.emplace_back(
                    (*leaf)[std::string_view("fieldName")].as_string().value_or(
                        ""));
            }
        }
        return names;
    }

    // 递归收集一个 section.entries[] 内（含 row 嵌套）的 commandId 叶子。
    // 用于断言 update section 含 checkUpdate/applyUpdate 命令叶子。
    std::vector<std::string> LayoutSectionCommandIds(
        const yyjson::value& section)
    {
        std::vector<std::string> ids;
        auto                     section_obj = section.as_object();
        if (!section_obj || !section_obj->contains(std::string_view("entries")))
        {
            return ids;
        }
        auto entries = (*section_obj)[std::string_view("entries")].as_array();
        if (!entries)
        {
            return ids;
        }
        std::function<void(const yyjson::value&)> walk =
            [&](const yyjson::value& node)
        {
            auto obj = node.as_object();
            if (!obj)
            {
                return;
            }
            if (obj->contains(std::string_view("commandId")))
            {
                ids.emplace_back(
                    (*obj)[std::string_view("commandId")].as_string().value_or(
                        ""));
            }
            if (obj->contains(std::string_view("row")))
            {
                if (auto row = (*obj)[std::string_view("row")].as_array())
                {
                    for (auto row_it = row->begin(); row_it != row->end();
                         ++row_it)
                    {
                        walk(*row_it);
                    }
                }
            }
        };
        for (auto it = entries->begin(); it != entries->end(); ++it)
        {
            walk(*it);
        }
        return ids;
    }
} // namespace

TEST_F(MaapiAuthoringFixture, ApplyPathFailurePersistsDiagnostics)
{
    auto session = CreateSession();
    auto missing_path =
        Das::Utils::PathToGenericUtf8(FixturePath("missing.jsonc"));
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
    // 失败语义：远端不落盘——无效路径不得写入 acceptedProperties，保持原值。
    EXPECT_NE(
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
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
    // interface_authoring.jsonc 无 setting，stage 是游离 global_option，
    // 不投影成根字段（PI v2.8.0 setting 才进设置页）。
    EXPECT_FALSE(FormSequenceHasField(document, "options.stage"));
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    // D-84-34/40: 严格 contract 不再支持 pi.tasks 批量替换；初始状态直接通过
    // session context 注入一个 DailyFarm 实例（带显式 itemId 方便后续
    // formSequenceItem-targeted setValue 寻址）。
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
        + R"("},"pi":{"orphanPaths":["pi.tasks:MissingTask"]}})");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_daily_farm\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}");

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
    // taskName。session context 的 task 显式提供 itemId。
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        catalog.options = options;
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

TEST_F(
    MaapiAuthoringFixture,
    ApplyChangeOmitsPortDerivationsFromAcceptedProperties)
{
    auto interface_path =
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));

    // 起始 session 已注入 DailyFarm 实例（带 itemId），避免依赖已删除的
    // pi.tasks 批量替换入口。
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
    // ApplyResult schema）。
    EXPECT_FALSE(obj->contains(std::string_view("dynamic_ports")));

    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    // D-A3（111-03，110.1 D-01③ 翻案）：InjectPortDerivations 摘除后
    // acceptedProperties 不再写 portDefinitions/portMap（隐式注入死代码清退，
    // 反向断言锁定无残留写入路径）。存量 acceptedProperties 残留两键无害
    // （生产零读取方），不做数据迁移（RESEARCH Runtime State ② 裁决）。PI
    // 声明输入域改经 document fields[] 候选流（input.<optionKey>，见
    // ProjectsPiInputDomainAsCandidateFields）。
    EXPECT_FALSE(accepted->contains(std::string_view("portDefinitions")));
    EXPECT_FALSE(accepted->contains(std::string_view("portMap")));
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
            // message 格式 "Missing translation for key '<key>'..."；用闭合
            // 单引号精确匹配，避免 field.resource 误命中
            // field.resourceVersion。
            for (const auto* key :
                 {"field.controller'",
                  "field.resource'",
                  "field.preset'",
                  "field.language'"})
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
    auto catalog = MakeI18nMockCatalog();
    const Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;

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

    // stage 是游离 global_option（未被任何 setting 引用），不投影成根字段。
    EXPECT_EQ(GetFieldLabel(document, "options.stage"), "");
    // stage/retry/notify 都是 task 级 option（DailyFarm.dto.option），归位到
    // items[].fields[]，label 走 PI i18n 解析。
    EXPECT_EQ(
        GetSequenceItemFieldLabel(document, "item_daily_farm", "options.stage"),
        "关卡");
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
            + value_path + R"(","value":)" + value_json_literal + "}}";
        auto change = Das::Utils::ParseYyjsonFromString(json);
        ASSERT_TRUE(change.has_value());
        ApplySetValueChange(settings, *change, &catalog);
    };

    apply("options.stage", "\"1-1\"");                  // select string
    apply("options.retry_text", "\"hello\"");           // input 单字段 string
    apply("options.retry_count", "42");                 // input 单字段 int
    apply("options.coord", R"({"x":"100","y":"200"})"); // input 多字段 obj
    apply("options.tags", "[\"small\"]");               // checkbox jsonArray
    apply("options.notify", "true");                    // switch bool

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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

            (*field)[std::string_view("name")].as_string().value_or(""));
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

                (*leaf)[std::string_view("fieldName")].as_string().value_or(
                    ""));
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
        command_ids.emplace(id_opt);
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
    catalog.interface_directory = DAS_PATH_LITERAL("C:/maapi/strict");
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
        + R"("},"pi":{"presetName":"DailyPreset"}})");

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

// D-84.2-04: 选自定义 preset（setValue presetName=<customId>）经
// ApplyCustomPreset 加载快照配置，成功递增 revision、无 error。此前 presetName
// 处理只查 PI catalog，选自定义 preset 会误报 "Unknown preset name"。
TEST_F(MaapiAuthoringFixture, SetValuePresetNameLoadsCustomPreset)
{
    auto interface_path =
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
        + "\"},\"pi\":{\"controllerName\":\"Android\","
          "\"resourceName\":\"Official\",\"presetName\":\"DailyPreset\","
          "\"language\":\"zh_cn\",\"tasks\":[{\"itemId\":\"seed_t1\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}");

    // 1) newPreset 创建快照（镜像 controllerName=Android），auto-select。
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"newPreset\"}}");
    ASSERT_EQ(session->ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    auto r1 = ReadJsonInterface(r1_json.Get());
    auto pi1 = AcceptedPiFromApplyResult(r1).as_object();
    ASSERT_TRUE(pi1.has_value());
    const auto preset_id = std::string(
        (*pi1)[std::string_view("presetName")].as_string().value_or(""));
    ASSERT_FALSE(preset_id.empty());
    ASSERT_EQ(DocumentRevisionFromApplyResult(r1), 1);

    // 2) 改 controllerName，使当前配置偏离快照。
    DasPtr<Das::ExportInterface::IDasJson> r2_json;
    auto                                   c2 = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"controllerName\","
        "\"value\":\"Win32\"}}");
    ASSERT_EQ(session->ApplyChange(c2.Get(), r2_json.Put()), DAS_S_OK);
    ASSERT_EQ(
        DocumentRevisionFromApplyResult(ReadJsonInterface(r2_json.Get())),
        2);

    // 3) 选自定义 preset → ApplyCustomPreset 应加载快照（controllerName 恢复
    //    为 Android）。此前这一步会报 "Unknown preset name: <preset_id>"。
    DasPtr<Das::ExportInterface::IDasJson> r3_json;
    auto                                   c3 = ParseDasJson(
        "{\"baseRevision\":2,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"presetName\",\"value\":\""
        + preset_id + "\"}}");
    ASSERT_EQ(session->ApplyChange(c3.Get(), r3_json.Put()), DAS_S_OK);
    auto r3 = ReadJsonInterface(r3_json.Get());

    auto valid = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(r3, 2);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;
    EXPECT_EQ(DocumentRevisionFromApplyResult(r3), 3);
    EXPECT_EQ(CountDiagnosticsBySeverity(r3, "error"), 0u);

    auto pi3 = AcceptedPiFromApplyResult(r3).as_object();
    ASSERT_TRUE(pi3.has_value());
    EXPECT_EQ(
        (*pi3)[std::string_view("presetName")].as_string().value_or(""),
        preset_id);
    // 快照恢复：controllerName 从 Win32 回到快照里的 Android。
    EXPECT_EQ(
        (*pi3)[std::string_view("controllerName")].as_string().value_or(""),
        "Android");
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    // flat pi seed：无 configurations，只有 PI 官方字段 + 一个 task 实例。
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
            std::string(R"({"baseRevision":0,"kind":")") + kind_str
            + R"(","payload":{}})";
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
    // mutation。ApplyInvokeCommandChange 接 session& 引用直读写
    // device_cache_， 零 T* 出参指针。
    AcceptedSettingsDto   candidate = settings;
    MaapiAuthoringSession session(candidate);
    auto                  new_preset_payload =
        Das::Utils::ParseYyjsonFromString(R"({"commandId":"newPreset"})");
    ASSERT_TRUE(new_preset_payload.has_value());
    std::vector<PiDiagnosticDto> invoke_diagnostics;
    auto                         invoke_result = ApplyInvokeCommandChange(
        session,
        candidate,
        *new_preset_payload,
        invoke_diagnostics);
    ASSERT_TRUE(invoke_result.ok);
    EXPECT_FALSE(invoke_result.command_result.has_value());

    const std::vector<PiDiagnosticDto> diagnostics;
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
// ============================================================================

namespace
{

    // 读 fixtures 字面量为 std::string，避免测试依赖生产 serializer 生成期望。
    std::optional<std::string> ReadFixtureText(std::string_view relative)
    {
        const std::ifstream ifs(
            FixturePath(std::string(relative)),
            std::ios::binary);
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

} // namespace

// Tracer 1 RED: 字面量 v28 PI → MaaPI Provider Get → 严格 84-01 Get validator
// 通过。 actionCatalog 必须同时包含可重复的 PI task 类型（D-84-40 同 actionId
// 可多次添加）。 formSequence.items 为空（fresh state）但 form view 已经定形。
TEST_F(MaapiAuthoringFixture, LiteralV28InterfaceProjectsStrictWire)
{
    auto fixture_text = ReadFixtureText("interface_authoring_v28.jsonc");
    ASSERT_TRUE(fixture_text.has_value()) << "fixture missing";
    auto interface_path = Das::Utils::PathToGenericUtf8(
        FixturePath("interface_authoring_v28.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
TEST_F(MaapiAuthoringFixture, FreshSessionOmitsFreeGlobalOptionFromRootFields)
{
    auto interface_path = Das::Utils::PathToGenericUtf8(
        FixturePath("interface_authoring_v28.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

    DasPtr<Das::ExportInterface::IDasJson> get_json;
    ASSERT_EQ(session->GetDocument({}, get_json.Put()), DAS_S_OK);
    auto envelope = ReadJsonInterface(get_json.Get());
    auto get_valid =
        Das::Core::TaskScheduler::ValidateAuthoringGetResult(envelope);
    EXPECT_TRUE(get_valid.valid) << "error_kind=" << get_valid.error_kind
                                 << " message=" << get_valid.message;

    auto document = DocumentOfGet(get_json.Get());
    // v28 fixture 的 stage 是游离 global_option（无 setting 引用），不投影成
    // 根字段。其默认值改由 pipeline_override 合并处理。document 仍须通过
    // schema validator（fieldLayout 全覆盖）。
    EXPECT_FALSE(FindRootField(document, "options.stage").has_value());
}

// UAT 端到端回归：用户通过 addFormSequenceItem 在 UI 加入 DailyFarm 后，
// Apply result 的 document.formSequence.items[0].fields[] 必须含 PI 接口
// 默认值（retry multi-input 的 times=3 / unit=second；stage=1-1 首个 case；
// use-medicine=[] 因 v28 fixture 无 default_case）。
TEST_F(MaapiAuthoringFixture, AddFormSequenceItemProjectsTaskOptionPiDefaults)
{
    auto interface_path = Das::Utils::PathToGenericUtf8(
        FixturePath("interface_authoring_v28.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
    auto interface_path = Das::Utils::PathToGenericUtf8(
        FixturePath("interface_authoring_v28.jsonc"));

    // 把字面量 fixture 装进 session context。interfacePath 用 v28 fixture
    // 的绝对路径覆盖（fixture 内的相对值只是占位）。
    auto parsed = ParseJsonFromStringStrict(*fixture_text);
    ASSERT_TRUE(parsed.has_value());
    auto root_obj = parsed->as_object();
    ASSERT_TRUE(root_obj.has_value());
    auto pi_obj = (*root_obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi_obj.has_value());

    std::ostringstream context_buf;
    context_buf << R"({"adapter":{"interfacePath":")" << interface_path
                << R"("},"pi":)";
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
    auto interface_path = Das::Utils::PathToGenericUtf8(
        FixturePath("interface_authoring_v28.jsonc"));

    std::ostringstream context_buf;
    context_buf << R"({"adapter":{"interfacePath":")" << interface_path
                << R"("},"pi":)";
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
    auto interface_path = Das::Utils::PathToGenericUtf8(
        FixturePath("interface_authoring_v28.jsonc"));
    auto parsed = ParseJsonFromStringStrict(*fixture_text);
    ASSERT_TRUE(parsed.has_value());
    auto root_obj = parsed->as_object();
    ASSERT_TRUE(root_obj.has_value());

    std::ostringstream context_buf;
    context_buf << R"({"adapter":{"interfacePath":")" << interface_path
                << R"("},"pi":)";
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

    // 返回 document.fields[] 中指定 name 的完整 field 描述符克隆（含
    // valueType/inputType/candidates 等），缺失返回 nullopt。供 schedule
    // 字段 valueType/inputType 断言使用。
    std::optional<yyjson::value> FindField(
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
            o.dto.default_cases.emplace_back("hard");
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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    auto session = CreateSession(
        R"({"adapter":{"interfacePath":")" + interface_path + "\"}}");

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

// ---------------------------------------------------------------------------
// Phase 86-06 (D-86-03/04/06): pretask/setting neutral root FieldDescriptor
// projection.
//
// pretask.option → root FieldDescriptor(pretask.<key>) with acceptedProperties
// persistence (mirrors custom_presets private storage).
// setting.option → root FieldDescriptor(settings.<key>) sharing global storage
// with global_option (D-86-04). Overlap keys (key referenced by BOTH
// global_option and setting) emit only settings.<key>, not options.<key>,
// so UI renders exactly one field per overlapping key.
//
// Business-neutrality (D-86-06): projections reuse existing FieldDescriptor +
// setValue + acceptedProperties private storage. Core contract untouched.
// ---------------------------------------------------------------------------

namespace
{
    // 构造一个覆盖 pretask/setting/global_option + 重叠键场景的 mock catalog：
    //   - stage 是 global_option 同时被 setting(performance) 引用（重叠键）
    //   - retry 仅在 setting(advanced) 中（setting-only 键）
    //   - shared_opt 仅在 global_option（游离 global_option，不被 setting
    //   引用）
    //   - bootstrap_mode 仅在 pretask(Bootstrap) 的 option 引用中
    // 预期 fields[]：游离 global_option 不投影成根字段
    //   - options.shared_opt NOT emitted（游离 global_option）
    //   - options.stage NOT emitted（被 setting 引用，改发 settings.stage）
    //   - settings.stage emitted
    //   - settings.retry emitted
    //   - pretask.bootstrap_mode emitted
    PiCatalog MakeNeutralProjectionMockCatalog()
    {
        using namespace Das::Plugins::DasMaaPi;
        PiCatalog catalog;
        catalog.name = "NeutralProjection";
        catalog.version = "1.0";

        PiTask task;
        task.dto.name = "DailyFarm";
        task.dto.entry = "StartDaily";
        catalog.tasks.push_back(std::move(task));

        auto make_select = [](std::string              name,
                              std::vector<std::string> case_names,
                              std::optional<std::vector<std::string>> defaults =
                                  std::nullopt) -> PiOption
        {
            PiOption o;
            o.dto.name = std::move(name);
            o.dto.type = "select";
            for (auto& cn : case_names)
            {
                PiCaseDto c;
                c.name = std::move(cn);
                o.dto.cases.push_back(std::move(c));
            }
            if (defaults)
            {
                o.dto.default_cases = std::move(*defaults);
            }
            return o;
        };

        const PiOption stage_option = make_select(
            "stage",
            {"1-1", "1-2"},
            std::vector<std::string>{"1-1"});
        const PiOption retry_option = make_select("retry", {"slow", "fast"});
        const PiOption shared_option = make_select("shared_opt", {"yes", "no"});
        const PiOption bootstrap_option =
            make_select("bootstrap_mode", {"safe", "fast"});

        catalog.options.push_back(stage_option);
        catalog.options.push_back(retry_option);
        catalog.options.push_back(shared_option);
        catalog.options.push_back(bootstrap_option);

        // global_option 引用 stage + shared_opt；stage 与 setting 重叠。
        catalog.global_options.push_back(stage_option);
        catalog.global_options.push_back(shared_option);

        PiSetting performance;
        performance.dto.name = "performance";
        performance.dto.option = {"stage"};
        catalog.settings.push_back(std::move(performance));

        PiSetting advanced;
        advanced.dto.name = "advanced";
        advanced.dto.option = {"retry"};
        catalog.settings.push_back(std::move(advanced));

        PiPretask bootstrap;
        bootstrap.dto.name = "Bootstrap";
        bootstrap.dto.exec = "tools/bootstrap.exe";
        bootstrap.dto.option = {"bootstrap_mode"};
        catalog.pretasks.push_back(std::move(bootstrap));

        return catalog;
    }

    // 计 document.fields[] 中满足 predicate 的 field 数量。
    std::size_t CountFieldsMatching(
        const yyjson::value&                             document,
        const std::function<bool(const yyjson::value&)>& pred)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("fields")))
        {
            return 0;
        }
        auto fields = (*obj)[std::string_view("fields")].as_array();
        if (!fields)
        {
            return 0;
        }
        std::size_t count = 0;
        for (auto&& it : *fields)
        {
            if (pred(it))
            {
                ++count;
            }
        }
        return count;
    }

    std::size_t CountFieldsWithName(
        const yyjson::value& document,
        std::string_view     name)
    {
        return CountFieldsMatching(
            document,
            [&](const yyjson::value& field)
            {
                auto fo = field.as_object();
                if (!fo)
                {
                    return false;
                }
                return (*fo)[std::string_view("name")].as_string().value_or("")
                       == name;
            });
    }

    std::size_t CountFieldsWithPrefix(
        const yyjson::value& document,
        std::string_view     prefix)
    {
        return CountFieldsMatching(
            document,
            [&](const yyjson::value& field)
            {
                auto fo = field.as_object();
                if (!fo)
                {
                    return false;
                }
                auto name =
                    (*fo)[std::string_view("name")].as_string().value_or("");
                return std::string_view(name).starts_with(prefix);
            });
    }

    // 嵌套 pretask option 场景（GameSetting 形态）：display 是 select，其
    // Window case 声明嵌套 option resolution（Borderless case 无嵌套）。
    // pretask(GameSetting) 的 dto.option 只声明顶层 region/display——嵌套
    // resolution 不在任何顶层列表里。
    PiCatalog MakeNestedPretaskMockCatalog()
    {
        using namespace Das::Plugins::DasMaaPi;
        PiCatalog catalog;
        catalog.name = "NestedPretask";
        catalog.version = "1.0";

        PiTask task;
        task.dto.name = "DailyFarm";
        task.dto.entry = "StartDaily";
        catalog.tasks.push_back(std::move(task));

        PiOption region;
        region.dto.name = "region";
        region.dto.type = "select";
        PiCaseDto region_cn;
        region_cn.name = "CN";
        PiCaseDto region_global;
        region_global.name = "Global";
        region.dto.cases = {region_cn, region_global};
        region.dto.default_cases = {"CN"};

        PiOption display;
        display.dto.name = "display";
        display.dto.type = "select";
        PiCaseDto display_window;
        display_window.name = "Window";
        display_window.option = {"resolution"};
        PiCaseDto display_borderless;
        display_borderless.name = "Borderless";
        display.dto.cases = {display_window, display_borderless};
        display.dto.default_cases = {"Window"};

        PiOption resolution;
        resolution.dto.name = "resolution";
        resolution.dto.type = "select";
        PiCaseDto res_720;
        res_720.name = "1280x720";
        PiCaseDto res_1080;
        res_1080.name = "1920x1080";
        resolution.dto.cases = {res_720, res_1080};
        resolution.dto.default_cases = {"1280x720"};

        catalog.options.push_back(region);
        catalog.options.push_back(display);
        catalog.options.push_back(resolution);

        PiPretask gamesetting;
        gamesetting.dto.name = "GameSetting";
        gamesetting.dto.exec = "tools/pre.exe";
        gamesetting.dto.option = {"region", "display"};
        gamesetting.dto.label = "⚙️ Game Settings";
        catalog.pretasks.push_back(std::move(gamesetting));

        return catalog;
    }
} // namespace

// Feature 1 RED: pretask.option 引用 option 必须投影为 root FieldDescriptor
// (pretask.<key>)。值取自 acceptedProperties.pi.pretasks[<id>].options。
TEST(MaapiProjectionTest, ProjectsPretaskOptionAsNeutralRootField)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeNeutralProjectionMockCatalog();

    AcceptedSettingsDto settings;
    // 用户已为 Bootstrap 选择 bootstrap_mode=fast。
    MaapiPiPretaskSettingsDto pretask_settings;
    pretask_settings.pretask_id = "Bootstrap";
    MaapiPiOptionSettingsDto bootstrap_value;
    bootstrap_value.option_name = "bootstrap_mode";
    bootstrap_value.kind = "select";
    bootstrap_value.selected_cases.emplace_back("fast");
    pretask_settings.options.push_back(std::move(bootstrap_value));
    settings.pi.pretasks.push_back(std::move(pretask_settings));

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    // pretask.bootstrap_mode root field 必须存在，且 value = "fast"。
    EXPECT_EQ(CountFieldsWithName(document, "pretask.bootstrap_mode"), 1u);
    auto value = FieldValueOf(document, "pretask.bootstrap_mode");
    ASSERT_TRUE(value.is_string())
        << "pretask.bootstrap_mode value must be string (select scalar IR)";
    EXPECT_EQ(value.as_string().value_or(""), "fast");
}

// Feature 1 RED: pretask.option 在 acceptedProperties 未初始化时（fresh
// session）仍应投影为 root FieldDescriptor，value 回退到 PI default_case /
// 首个 case。
TEST(MaapiProjectionTest, ProjectsPretaskOptionWithPiDefaultWhenUnset)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                      catalog = MakeNeutralProjectionMockCatalog();
    const AcceptedSettingsDto settings; // fresh — no pretasks

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    EXPECT_EQ(CountFieldsWithName(document, "pretask.bootstrap_mode"), 1u);
    auto value = FieldValueOf(document, "pretask.bootstrap_mode");
    ASSERT_TRUE(value.is_string());
    // bootstrap_mode 的 default_cases 未设置，应回退首个 case "safe"。
    EXPECT_EQ(value.as_string().value_or(""), "safe");
}

// Feature 1 RED: setValue target=document field=pretask.<key> 必须持久化到
// acceptedProperties.pi.pretasks[<id>].options。
TEST(MaapiProjectionTest, ApplyDocumentSetValueOnPretaskFieldPersistsValue)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeNeutralProjectionMockCatalog();
    AcceptedSettingsDto settings;

    std::vector<PiDiagnosticDto> diagnostics;
    auto                         payload = Das::Utils::ParseYyjsonFromString(
                                               R"({"target":{"kind":"document"},)"
                                               R"("fieldName":"pretask.bootstrap_mode",)"
                                               R"("value":"fast"})")
                                               .value_or(yyjson::value(nullptr));
    const bool                   ok =
        ApplyStrictSetValueChange(settings, payload, &catalog, diagnostics);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(diagnostics.empty());

    // 持久化路径：settings.pi.pretasks 里应有 Bootstrap 条目，其 options 含
    // bootstrap_mode="fast"。
    auto it = std::find_if(
        settings.pi.pretasks.begin(),
        settings.pi.pretasks.end(),
        [&](const MaapiPiPretaskSettingsDto& p)
        { return p.pretask_id == "Bootstrap"; });
    ASSERT_NE(it, settings.pi.pretasks.end());
    auto opt_it = std::find_if(
        it->options.begin(),
        it->options.end(),
        [&](const MaapiPiOptionSettingsDto& o)
        { return o.option_name == "bootstrap_mode"; });
    ASSERT_NE(opt_it, it->options.end());
    ASSERT_EQ(opt_it->selected_cases.size(), 1u);
    EXPECT_EQ(opt_it->selected_cases[0], "fast");
}

// 嵌套 option（case.option）按激活 case 递归投影：fresh session 下
// display 的 default case Window 激活 → pretask.resolution 字段出现；
// 用户切到无嵌套的 case（Borderless）→ 重投影后 resolution 字段消失
// （服务端重投影，前端零条件可见性）。
TEST(MaapiProjectionTest, ProjectsNestedPretaskOptionOnActivatedCase)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeNestedPretaskMockCatalog();

    {
        const AcceptedSettingsDto settings;
        auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
        auto validate =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                    << " message=" << validate.message;
        EXPECT_EQ(CountFieldsWithName(document, "pretask.region"), 1u);
        EXPECT_EQ(CountFieldsWithName(document, "pretask.display"), 1u);
        EXPECT_EQ(CountFieldsWithName(document, "pretask.resolution"), 1u);
    }

    AcceptedSettingsDto       borderless_settings;
    MaapiPiPretaskSettingsDto borderless_pretask;
    borderless_pretask.pretask_id = "GameSetting";
    MaapiPiOptionSettingsDto display_value;
    display_value.option_name = "display";
    display_value.kind = "select";
    display_value.selected_cases.emplace_back("Borderless");
    borderless_pretask.options.push_back(std::move(display_value));
    borderless_settings.pi.pretasks.push_back(std::move(borderless_pretask));

    auto document =
        ProjectAuthoringDocument(borderless_settings, &catalog, {}, 0);
    EXPECT_EQ(CountFieldsWithName(document, "pretask.region"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "pretask.display"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "pretask.resolution"), 0u);
}

// 嵌套 pretask option 的 setValue 路由：pretask.<nestedKey> 必须沿 option
// 树找到声明它的 pretask（顶层 dto.option 列表里没有嵌套键，此前会报
// setValueFieldNotDeclaredOnAnyPretask 拒绝）。
TEST(
    MaapiProjectionTest,
    ApplyDocumentSetValueOnNestedPretaskFieldPersistsValue)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeNestedPretaskMockCatalog();
    AcceptedSettingsDto settings;

    std::vector<PiDiagnosticDto> diagnostics;
    auto                         payload = Das::Utils::ParseYyjsonFromString(
                                               R"({"target":{"kind":"document"},)"
                                               R"("fieldName":"pretask.resolution",)"
                                               R"("value":"1920x1080"})")
                                               .value_or(yyjson::value(nullptr));
    const bool                   ok =
        ApplyStrictSetValueChange(settings, payload, &catalog, diagnostics);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(diagnostics.empty());

    auto it = std::find_if(
        settings.pi.pretasks.begin(),
        settings.pi.pretasks.end(),
        [&](const MaapiPiPretaskSettingsDto& p)
        { return p.pretask_id == "GameSetting"; });
    ASSERT_NE(it, settings.pi.pretasks.end());
    auto opt_it = std::find_if(
        it->options.begin(),
        it->options.end(),
        [&](const MaapiPiOptionSettingsDto& o)
        { return o.option_name == "resolution"; });
    ASSERT_NE(opt_it, it->options.end());
    ASSERT_EQ(opt_it->selected_cases.size(), 1u);
    EXPECT_EQ(opt_it->selected_cases[0], "1920x1080");
}

// pretask 字段独立 fieldLayout section：id=pretask_<stableId>、label=
// 作者 label（i18n/字面）、entries 恰好覆盖该 pretask 的全部 pretask.*
// 字段（含嵌套）；这些字段不得再混入 project section。
TEST(MaapiProjectionTest, PretaskFieldsGetDedicatedFieldLayoutSection)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                      catalog = MakeNestedPretaskMockCatalog();
    const AcceptedSettingsDto settings;

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    auto obj = document.as_object();
    ASSERT_TRUE(obj);
    ASSERT_TRUE(obj->contains(std::string_view("fieldLayout")));
    auto layout = (*obj)[std::string_view("fieldLayout")].as_array();
    ASSERT_TRUE(layout);

    bool                  found_pretask_section = false;
    std::string           pretask_label;
    std::set<std::string> pretask_entry_names;
    bool                  found_project_section = false;
    std::set<std::string> project_field_names;
    for (auto&& section : *layout)
    {
        auto so = section.as_object();
        if (!so)
        {
            continue;
        }
        const auto id = (*so)[std::string_view("id")].as_string().value_or("");
        if (id == "pretask_GameSetting")
        {
            found_pretask_section = true;
            pretask_label = std::string(
                (*so)[std::string_view("label")].as_string().value_or(""));
            auto entries = (*so)[std::string_view("entries")].as_array();
            ASSERT_TRUE(entries);
            for (auto&& entry : *entries)
            {
                auto eo = entry.as_object();
                ASSERT_TRUE(eo);
                pretask_entry_names.insert(
                    std::string((*eo)[std::string_view("fieldName")]
                                    .as_string()
                                    .value_or("")));
            }
        }
        if (id == "project")
        {
            found_project_section = true;
            auto entries = (*so)[std::string_view("entries")].as_array();
            ASSERT_TRUE(entries);
            for (auto&& entry : *entries)
            {
                auto eo = entry.as_object();
                if (!eo || !eo->contains(std::string_view("fieldName")))
                {
                    continue;
                }
                project_field_names.insert(
                    std::string((*eo)[std::string_view("fieldName")]
                                    .as_string()
                                    .value_or("")));
            }
        }
    }
    ASSERT_TRUE(found_pretask_section)
        << "fieldLayout must contain a dedicated pretask section";
    ASSERT_TRUE(found_project_section);

    EXPECT_EQ(pretask_label, "⚙️ Game Settings");
    EXPECT_EQ(
        pretask_entry_names,
        (std::set<std::string>{
            "pretask.region",
            "pretask.display",
            "pretask.resolution"}));

    for (const auto* leaked :
         {"pretask.region", "pretask.display", "pretask.resolution"})
    {
        EXPECT_EQ(project_field_names.count(leaked), 0u)
            << leaked << " must not stay in the project section";
    }
}

// Feature 2 RED: setting 引用 option 必须投影为 root FieldDescriptor
// (settings.<key>)，与既有 global_option 根字段并列。
TEST(MaapiProjectionTest, ProjectsSettingOptionAsNeutralRootField)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                      catalog = MakeNeutralProjectionMockCatalog();
    const AcceptedSettingsDto settings;

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    // settings.stage + settings.retry 必须存在。
    EXPECT_EQ(CountFieldsWithName(document, "settings.stage"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "settings.retry"), 1u);
    // stage default_cases=["1-1"] → 默认值 1-1。
    auto stage_val = FieldValueOf(document, "settings.stage");
    ASSERT_TRUE(stage_val.is_string());
    EXPECT_EQ(stage_val.as_string().value_or(""), "1-1");
}

// Feature 2 RED (首轮审计主要点)：重叠键（同时被 global_option 与 setting
// 引用）必须只发 settings.<key> 一个根字段，global_option 循环跳过该键。
// UI 渲染计数 = 1（不重复）。
TEST(MaapiProjectionTest, OverlapKeyEmittedAsSettingsOnlyNotOptions)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                      catalog = MakeNeutralProjectionMockCatalog();
    const AcceptedSettingsDto settings;

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    // stage ∈ S（被 setting 引用）→ options.stage 必须不存在。
    EXPECT_EQ(CountFieldsWithName(document, "options.stage"), 0u)
        << "overlap key must NOT emit options.<key>";
    // settings.stage 必须存在。
    EXPECT_EQ(CountFieldsWithName(document, "settings.stage"), 1u)
        << "overlap key must emit settings.<key>";

    // 全文 stage 总渲染数：唯一一个根字段 settings.stage
    // （actionCatalog.entries/groups 不引用 option keys，故 document.fields
    // 是唯一渲染点）。计数 = 1。
    auto stage_in_fields = CountFieldsMatching(
        document,
        [](const yyjson::value& field)
        {
            auto fo = field.as_object();
            if (!fo)
            {
                return false;
            }
            auto name =
                (*fo)[std::string_view("name")].as_string().value_or("");
            return std::string_view(name).find("stage") != std::string::npos;
        });
    EXPECT_EQ(stage_in_fields, 1u)
        << "stage should render exactly once across document.fields";

    // shared_opt 是游离 global_option（未被任何 setting 引用），不投影成根
    // 字段（PI v2.8.0 setting 才进设置页）。
    EXPECT_EQ(CountFieldsWithName(document, "options.shared_opt"), 0u);
    // shared_opt 不该走 settings.* 路径。
    EXPECT_EQ(CountFieldsWithName(document, "settings.shared_opt"), 0u);
}

// 根级 fieldLayout[] 把 fields[] 按分区编组：项目设置 section（adapter/* +
// controller/resource/preset/language）+ 每个 pretask 独立一 section
// （pretask_<stableId>）+ 每个 catalog->settings 元素独立一 section
// （settings.<opt>）。叶子 fieldName 集合 == fields[].name
// 集合（全覆盖、唯一），schema validator apply 路径通过。游离 global_option
// 不出现在 fields[]/fieldLayout（PI v2.8.0 setting 才是进设置页的声明）。
TEST(
    MaapiProjectionTest,
    RootFieldLayoutPartitionsFieldsIntoProjectAndSettingSections)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeNeutralProjectionMockCatalog();
    // 已配置 project_interface 的 entry（更新分区按配置存在性投影）。
    AcceptedSettingsDto settings;
    settings.adapter.interface_path =
        "C:/das-authoring-layout-sections/interface.json";

    std::vector<PiDiagnosticDto> diags;
    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0, &diags);

    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    ASSERT_TRUE(doc_obj->contains(std::string_view("fieldLayout")));
    auto layout = (*doc_obj)[std::string_view("fieldLayout")].as_array();
    ASSERT_TRUE(layout.has_value());

    // 项目设置 section（顺序第一）。defaultExpanded=true。
    // D-89-09 增量：mirrorchyanCdk / proxyUrl / updateChannel / 版本字段
    // 已独立到 update section，project 只含项目级字段。
    auto project_section = FindLayoutSection(document, "project");
    ASSERT_TRUE(project_section.has_value());
    auto project_fields = LayoutSectionFieldNames(*project_section);
    ASSERT_EQ(project_fields.size(), 6u);
    EXPECT_EQ(project_fields[0], "adapter.interfacePath");
    EXPECT_EQ(project_fields[1], "adapter.executionPolicy.failFast");
    EXPECT_EQ(project_fields[2], "controllerName");
    EXPECT_EQ(project_fields[3], "resourceName");
    EXPECT_EQ(project_fields[4], "presetName");
    EXPECT_EQ(project_fields[5], "language");

    // pretask 独立 section：Bootstrap 的 option 字段从 project section 移
    // 到专属分区（label 来自 pretask 作者 label，此处字面无 i18n 原样）。
    auto pretask_section = FindLayoutSection(document, "pretask_Bootstrap");
    ASSERT_TRUE(pretask_section.has_value());
    auto pretask_fields = LayoutSectionFieldNames(*pretask_section);
    ASSERT_EQ(pretask_fields.size(), 1u);
    EXPECT_EQ(pretask_fields[0], "pretask.bootstrap_mode");
    auto pretask_section_obj = pretask_section->as_object();
    ASSERT_TRUE(pretask_section_obj.has_value());
    EXPECT_TRUE((*pretask_section_obj)[std::string_view("defaultExpanded")]
                    .as_bool()
                    .value_or(false));

    auto project_section_obj = project_section->as_object();
    ASSERT_TRUE(project_section_obj.has_value());
    EXPECT_TRUE((*project_section_obj)[std::string_view("defaultExpanded")]
                    .as_bool()
                    .value_or(false));

    // 更新 section（顺序第二）：CDK / 代理 / 检查通道 + 两个只读版本字段。
    // defaultExpanded=true，与项目设置 section 一致。
    auto update_section = FindLayoutSection(document, "update");
    ASSERT_TRUE(update_section.has_value());
    auto update_fields = LayoutSectionFieldNames(*update_section);
    ASSERT_EQ(update_fields.size(), 5u);
    EXPECT_EQ(update_fields[0], "adapter.mirrorchyanCdk");
    EXPECT_EQ(update_fields[1], "adapter.proxyUrl");
    EXPECT_EQ(update_fields[2], "adapter.updateChannel");
    EXPECT_EQ(update_fields[3], "resourceVersion");
    EXPECT_EQ(update_fields[4], "maafwVersion");
    auto update_section_obj = update_section->as_object();
    ASSERT_TRUE(update_section_obj.has_value());
    EXPECT_TRUE((*update_section_obj)[std::string_view("defaultExpanded")]
                    .as_bool()
                    .value_or(false));

    // setting section: performance → settings.stage。
    auto perf_section = FindLayoutSection(document, "setting_performance");
    ASSERT_TRUE(perf_section.has_value());
    auto perf_fields = LayoutSectionFieldNames(*perf_section);
    ASSERT_EQ(perf_fields.size(), 1u);
    EXPECT_EQ(perf_fields[0], "settings.stage");

    // setting section: advanced → settings.retry。
    auto adv_section = FindLayoutSection(document, "setting_advanced");
    ASSERT_TRUE(adv_section.has_value());
    auto adv_fields = LayoutSectionFieldNames(*adv_section);
    ASSERT_EQ(adv_fields.size(), 1u);
    EXPECT_EQ(adv_fields[0], "settings.retry");

    // 全覆盖 + 唯一：layout 叶子 fieldName 集合 == fields[].name 集合。
    auto                        layout_names = LayoutLeafFieldNames(document);
    auto                        field_names = RootFieldNames(document);
    const std::set<std::string> layout_set(
        layout_names.begin(),
        layout_names.end());
    const std::set<std::string> field_set(
        field_names.begin(),
        field_names.end());
    EXPECT_EQ(layout_set, field_set)
        << "fieldLayout must cover every root field exactly once";
    EXPECT_EQ(layout_set.size(), layout_names.size())
        << "fieldLayout must not duplicate any field reference";

    // 游离 global_option 不出现在 fields[]/fieldLayout。
    EXPECT_EQ(CountFieldsWithName(document, "options.shared_opt"), 0u);
    EXPECT_EQ(CountFieldsWithName(document, "options.stage"), 0u);
}

// update section 内含 {row:[{commandId:checkUpdate},{commandId:applyUpdate}]}
// 横排命令按钮组，与 update 字段同 section 渲染。
// commands[] 投影仍 emit checkUpdate/applyUpdate/newPreset 三个 id（disabled
// 状态投影机制保持）。回归守卫：refactor 后 commands 投影 + checkUpdate
// commandResult 不破。
TEST(
    MaapiProjectionTest,
    UpdateSectionLayoutContainsCommandLeafAndCommandsStillEmitted)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeNeutralProjectionMockCatalog();
    // 已配置 project_interface 的 entry（更新命令按配置存在性投影）。
    AcceptedSettingsDto settings;
    settings.adapter.interface_path =
        "C:/das-authoring-update-commands/interface.json";

    std::vector<PiDiagnosticDto> diags;
    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0, &diags);

    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    ASSERT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    auto update_section = FindLayoutSection(document, "update");
    ASSERT_TRUE(update_section.has_value());

    // update section.entries 内的 commandId 叶子集合（含 row 嵌套）。
    auto command_ids = LayoutSectionCommandIds(*update_section);
    ASSERT_EQ(command_ids.size(), 2u);
    EXPECT_EQ(command_ids[0], "checkUpdate");
    EXPECT_EQ(command_ids[1], "applyUpdate");

    // commands[] 投影仍 emit 三个 id（归位 ≠ 删 commands
    // 投影，只是改渲染入口）。
    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    ASSERT_TRUE(doc_obj->contains(std::string_view("commands")));
    auto commands = (*doc_obj)[std::string_view("commands")].as_array();
    ASSERT_TRUE(commands.has_value());
    std::set<std::string> emitted_ids;
    for (auto it = commands->begin(); it != commands->end(); ++it)
    {
        auto cmd_obj = it->as_object();
        if (!cmd_obj)
        {
            continue;
        }
        emitted_ids.insert(
            std::string(
                (*cmd_obj)[std::string_view("id")].as_string().value_or("")));
    }
    EXPECT_EQ(emitted_ids.count("checkUpdate"), 1u);
    EXPECT_EQ(emitted_ids.count("applyUpdate"), 1u);
    EXPECT_EQ(emitted_ids.count("newPreset"), 1u);
}

// catalog 缺席（无 PI 接口）且 project_interface 未配置（新建 entry 的
// 初始形态）时，fieldLayout 覆盖 adapter 两字段；更新分区（CDK / 代理 /
// 通道 + 版本只读字段 + checkUpdate/applyUpdate 命令）不预置——空任务是
// 干净空表单，用户实测「双击新建 entry 默认带上更新内容」的回归守卫。
TEST(MaapiProjectionTest, RootFieldLayoutCoversAdapterFieldsWhenCatalogAbsent)
{
    using namespace Das::Plugins::DasMaaPi;

    const AcceptedSettingsDto settings;
    auto document = ProjectAuthoringDocument(settings, nullptr, {}, 0);

    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    auto project_section = FindLayoutSection(document, "project");
    ASSERT_TRUE(project_section.has_value());
    auto project_fields = LayoutSectionFieldNames(*project_section);
    // mirrorchyanCdk / proxyUrl / updateChannel + 版本字段独立到 update
    // section；project 只含 adapter 两字段（interfacePath / executionPolicy）。
    ASSERT_EQ(project_fields.size(), 2u);
    EXPECT_EQ(project_fields[0], "adapter.interfacePath");
    EXPECT_EQ(project_fields[1], "adapter.executionPolicy.failFast");

    // project_interface 未配置 → 更新分区整体省略（fields / fieldLayout
    // section / commands 三处载体都不预置）。
    EXPECT_FALSE(FindLayoutSection(document, "update").has_value());
    for (const std::string_view update_field :
         {"adapter.mirrorchyanCdk",
          "adapter.proxyUrl",
          "adapter.updateChannel",
          "resourceVersion",
          "maafwVersion"})
    {
        EXPECT_FALSE(FindRootField(document, update_field).has_value())
            << "update field " << update_field
            << " must not be projected for unconfigured entry";
    }
    {
        std::set<std::string> emitted_command_ids;
        auto                  doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        if (auto commands = (*doc_obj)[std::string_view("commands")].as_array())
        {
            for (auto it = commands->begin(); it != commands->end(); ++it)
            {
                auto cmd_obj = it->as_object();
                if (!cmd_obj)
                {
                    continue;
                }
                emitted_command_ids.insert(
                    std::string(
                        (*cmd_obj)[std::string_view("id")].as_string().value_or(
                            "")));
            }
        }
        EXPECT_EQ(emitted_command_ids.count("checkUpdate"), 0u);
        EXPECT_EQ(emitted_command_ids.count("applyUpdate"), 0u);
        EXPECT_EQ(emitted_command_ids.count("newPreset"), 1u);
    }

    auto                        layout_names = LayoutLeafFieldNames(document);
    auto                        field_names = RootFieldNames(document);
    const std::set<std::string> layout_set(
        layout_names.begin(),
        layout_names.end());
    const std::set<std::string> field_set(
        field_names.begin(),
        field_names.end());
    EXPECT_EQ(layout_set, field_set)
        << "fieldLayout must cover every root field exactly once";
}

// ============================================================================
// Phase 87-03 — BuildDocument 把 ui.values["sectionExpand.<id>"] 合并进
// fieldLayout[].defaultExpanded（B 方案：MaaPI 投影层合并，Get 响应不加
// acceptedProperties）。前端直接读 section.DefaultExpanded，不解析 raw kv。
// ============================================================================

// REQ-MaaPI-merge 用例 1（TrueOverride）：原始 default_expand=false 的 setting
// section，ui.values["sectionExpand.<id>"]=true → defaultExpanded==true。
TEST(MaapiAuthoringDefaultExpandedMerge, TrueOverrideRestoresExpandedPreference)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeNeutralProjectionMockCatalog();
    ASSERT_FALSE(catalog.settings.empty());
    catalog.settings[0].dto.default_expand = false;

    AcceptedSettingsDto settings;
    settings.ui.values["sectionExpand.setting_performance"] =
        yyjson::value(true);

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto section = FindLayoutSection(document, "setting_performance");
    ASSERT_TRUE(section.has_value());
    auto section_obj = section->as_object();
    ASSERT_TRUE(section_obj.has_value());
    EXPECT_TRUE(
        (*section_obj)[std::string_view("defaultExpanded")].as_bool().value_or(
            false));
}

// REQ-MaaPI-merge 用例 2（FalseOverride）：原始 default_expand=true 的 setting
// section，ui.values["sectionExpand.<id>"]=false → defaultExpanded==false。
TEST(
    MaapiAuthoringDefaultExpandedMerge,
    FalseOverrideRestoresCollapsedPreference)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeNeutralProjectionMockCatalog();
    ASSERT_GE(catalog.settings.size(), 2u);
    catalog.settings[1].dto.default_expand = true;

    AcceptedSettingsDto settings;
    settings.ui.values["sectionExpand.setting_advanced"] = yyjson::value(false);

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto section = FindLayoutSection(document, "setting_advanced");
    ASSERT_TRUE(section.has_value());
    auto section_obj = section->as_object();
    ASSERT_TRUE(section_obj.has_value());
    EXPECT_FALSE(
        (*section_obj)[std::string_view("defaultExpanded")].as_bool().value_or(
            true));
}

// REQ-MaaPI-merge 用例 3（MissingKeyDefault）：ui.values 无对应键 →
// 用原始默认。
TEST(MaapiAuthoringDefaultExpandedMerge, MissingKeyFallsBackToOriginalDefault)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeNeutralProjectionMockCatalog();
    ASSERT_FALSE(catalog.settings.empty());
    catalog.settings[0].dto.default_expand = false;
    ASSERT_GE(catalog.settings.size(), 2u);
    catalog.settings[1].dto.default_expand = true;

    AcceptedSettingsDto settings; // ui.values 空 → 全部走原始默认

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto perf = FindLayoutSection(document, "setting_performance");
    ASSERT_TRUE(perf.has_value());
    EXPECT_FALSE((*perf->as_object())[std::string_view("defaultExpanded")]
                     .as_bool()
                     .value_or(true));

    auto adv = FindLayoutSection(document, "setting_advanced");
    ASSERT_TRUE(adv.has_value());
    EXPECT_TRUE((*adv->as_object())[std::string_view("defaultExpanded")]
                    .as_bool()
                    .value_or(false));

    // 项目设置 section 原始硬编码 true，不受 ui.values 缺失影响。
    auto project = FindLayoutSection(document, "project");
    ASSERT_TRUE(project.has_value());
    EXPECT_TRUE((*project->as_object())[std::string_view("defaultExpanded")]
                    .as_bool()
                    .value_or(false));
}

// REQ-MaaPI-merge 用例 4（NonBoolIgnored）：ui.values 存在但类型非 bool
// （字符串/数字）→ 安全 fallback 用原始默认（cpp_yyjson safe cast 失败不抛）。
TEST(MaapiAuthoringDefaultExpandedMerge, NonBoolValueIgnoredFallsBackToOriginal)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeNeutralProjectionMockCatalog();
    ASSERT_FALSE(catalog.settings.empty());
    catalog.settings[0].dto.default_expand = false;
    ASSERT_GE(catalog.settings.size(), 2u);
    catalog.settings[1].dto.default_expand = true;

    AcceptedSettingsDto settings;
    auto str_val = Das::Utils::ParseYyjsonFromString(R"("true")");
    ASSERT_TRUE(str_val.has_value());
    settings.ui.values["sectionExpand.setting_performance"] =
        std::move(*str_val);
    auto num_val = Das::Utils::ParseYyjsonFromString("1");
    ASSERT_TRUE(num_val.has_value());
    settings.ui.values["sectionExpand.setting_advanced"] = std::move(*num_val);

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto perf = FindLayoutSection(document, "setting_performance");
    ASSERT_TRUE(perf.has_value());
    EXPECT_FALSE((*perf->as_object())[std::string_view("defaultExpanded")]
                     .as_bool()
                     .value_or(true));

    auto adv = FindLayoutSection(document, "setting_advanced");
    ASSERT_TRUE(adv.has_value());
    EXPECT_TRUE((*adv->as_object())[std::string_view("defaultExpanded")]
                    .as_bool()
                    .value_or(false));
}

// REQ-MaaPI-merge round-trip：SerializeAcceptedSettings → ParseAcceptedSettings
// → ProjectAuthoringDocument 必须保留 ui.values 并合并进 defaultExpanded。此前
// Serialize 把 ui.values 扁平铺到 ui、Parse 查 ui.values 读不回 → 合并失效（Get
// 在 Apply 后 evict session 从 acceptedProperties 重建走的正是这条
// round-trip）。锁定 序列化/反序列化对 ui.values 对称，防回归。
TEST(
    MaapiAuthoringDefaultExpandedMerge,
    SerializeParseRoundTripPreservesUiValuesAndMerges)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeNeutralProjectionMockCatalog();
    ASSERT_FALSE(catalog.settings.empty());
    catalog.settings[0].dto.default_expand = true;

    AcceptedSettingsDto settings;
    settings.ui.values["sectionExpand.setting_performance"] =
        yyjson::value(false);

    // 模拟 Apply 持久化 acceptedProperties 后，Get 重建 session 从它解析
    // settings。
    auto accepted_json = SerializeAcceptedSettings(settings);
    auto restored = ParseAcceptedSettings(accepted_json);

    auto document = ProjectAuthoringDocument(restored, &catalog, {}, 0);

    auto section = FindLayoutSection(document, "setting_performance");
    ASSERT_TRUE(section.has_value());
    auto section_obj = section->as_object();
    ASSERT_TRUE(section_obj.has_value());
    EXPECT_FALSE(
        (*section_obj)[std::string_view("defaultExpanded")].as_bool().value_or(
            true))
        << "round-trip 后 ui.values 必须保留并合并进 defaultExpanded";
}

// REQ-MaaPI-W3（B 方案核心约束）：Get 响应 envelope 不含顶层
// acceptedProperties。 ui.values 合并后 Get 路径产出的 envelope 仍是 {document,
// diagnostics}，前端通过 document.fieldLayout[].defaultExpanded 拿偏好，不读
// raw kv。显式 negative assertion 锁定此不变量，防后续误在 Get envelope 暴露
// acceptedProperties。
TEST(
    MaapiAuthoringGetEnvelopeOmitsAcceptedProperties,
    GetPathEnvelopeHasNoAcceptedPropertiesTopLevel)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeNeutralProjectionMockCatalog();
    AcceptedSettingsDto settings;
    settings.ui.values["sectionExpand.setting_performance"] =
        yyjson::value(true);

    std::vector<PiDiagnosticDto> diagnostics;
    auto                         document =
        ProjectAuthoringDocument(settings, &catalog, diagnostics, 0);

    // 构造 Get envelope（镜像 MaapiAuthoringSession::GetDocument: {document,
    // diagnostics}）。
    yyjson::value envelope(Das::Utils::MakeYyjsonObject());
    auto          env_obj = envelope.as_object();
    (*env_obj)[std::string_view("document")] =
        Das::Utils::CloneYyjsonValue(document);
    auto empty_diag = Das::Utils::ParseYyjsonFromString("[]");
    ASSERT_TRUE(empty_diag.has_value());
    (*env_obj)[std::string_view("diagnostics")] = std::move(*empty_diag);

    // W3 显式 negative assertion：Get envelope 不含 acceptedProperties
    // 顶层字段。
    EXPECT_FALSE(env_obj->contains(std::string_view("acceptedProperties")));

    // document 本身也不含 acceptedProperties（document 是 UI
    // IR，非持久化形态）。
    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    EXPECT_FALSE(doc_obj->contains(std::string_view("acceptedProperties")));
}

// Feature 2 RED: setValue target=document field=settings.<key> 必须持久化到
// acceptedProperties.pi.global_options（setting 与 global_option 共享
// global 级存储，D-86-04）。
TEST(MaapiProjectionTest, ApplyDocumentSetValueOnSettingsFieldPersistsGlobal)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeNeutralProjectionMockCatalog();
    AcceptedSettingsDto settings;

    std::vector<PiDiagnosticDto> diagnostics;
    auto                         payload = Das::Utils::ParseYyjsonFromString(
                                               R"({"target":{"kind":"document"},)"
                                               R"("fieldName":"settings.stage",)"
                                               R"("value":"1-2"})")
                                               .value_or(yyjson::value(nullptr));
    const bool                   ok =
        ApplyStrictSetValueChange(settings, payload, &catalog, diagnostics);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(diagnostics.empty());

    // 持久化路径：setting 与 global_option 共享 global_options 存储。
    auto it = std::find_if(
        settings.pi.global_options.begin(),
        settings.pi.global_options.end(),
        [&](const MaapiPiOptionSettingsDto& o)
        { return o.option_name == "stage"; });
    ASSERT_NE(it, settings.pi.global_options.end());
    ASSERT_EQ(it->selected_cases.size(), 1u);
    EXPECT_EQ(it->selected_cases[0], "1-2");
}

namespace
{
    // D-A3（111-03）PI 输入域投影测试的 mock catalog：首个 task 声明四类
    // option（select / input(int) / checkbox / switch），覆盖 MapOptionType
    // 的声明形态映射面。
    PiCatalog MakePiInputDomainMockCatalog()
    {
        using namespace Das::Plugins::DasMaaPi;
        PiCatalog catalog;
        catalog.name = "PiInputDomain";
        catalog.version = "1.0";

        PiTask task;
        task.dto.name = "DailyFarm";
        task.dto.entry = "StartDaily";
        task.dto.option = {"stage", "retry", "use-medicine", "notify"};
        catalog.tasks.push_back(std::move(task));

        auto make_option = [](std::string name, std::string type) -> PiOption
        {
            PiOption o;
            o.dto.name = std::move(name);
            o.dto.type = std::move(type);
            PiCaseDto c;
            c.name = "1-1";
            o.dto.cases.push_back(std::move(c));
            return o;
        };

        PiOption stage = make_option("stage", "select");
        PiOption medicine = make_option("use-medicine", "checkbox");
        PiOption notify = make_option("notify", "switch");

        PiOption retry;
        retry.dto.name = "retry";
        retry.dto.type = "input";
        PiInputDto times;
        times.name = "times";
        times.default_value = "3";
        times.pipeline_type = "int";
        retry.dto.inputs.push_back(std::move(times));

        catalog.options.push_back(std::move(stage));
        catalog.options.push_back(std::move(retry));
        catalog.options.push_back(std::move(medicine));
        catalog.options.push_back(std::move(notify));
        return catalog;
    }

    AcceptedSettingsDto MakePiInputDomainSettings()
    {
        using namespace Das::Plugins::DasMaaPi;
        AcceptedSettingsDto    settings;
        MaapiPiTaskSettingsDto task_settings;
        task_settings.item_id = "item_daily_farm";
        task_settings.task_name = "DailyFarm";
        task_settings.enabled = true;
        settings.pi.tasks.push_back(std::move(task_settings));
        return settings;
    }
} // namespace

// D-A3（111-03）：PI 声明输入域（首个 task 声明的 option）投影为 root 候选
// 字段 input.<optionKey>——与 config 域字段同一 fields[] 候选流（隐式注入
// 摘除后的显式可见通道，111-04/111-06 暴露面板零特判消费）。类型经
// MapOptionType 声明形态：select→(4,1)、input(int)→(0,0)、checkbox→(32,2)、
// switch→(8,0)。
TEST(MaapiProjectionTest, ProjectsPiInputDomainAsCandidateFields)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakePiInputDomainMockCatalog();
    AcceptedSettingsDto settings = MakePiInputDomainSettings();

    std::vector<PiDiagnosticDto> diags;
    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0, &diags);
    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    // 四类 option 各一候选（option 粒度；select case 不拆子字段）。
    EXPECT_EQ(CountFieldsWithName(document, "input.stage"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "input.retry"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "input.use-medicine"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "input.notify"), 1u);

    // 类型映射断言（照既有 valueType/inputType 断言形态）。
    auto stage_field = FindRootField(document, "input.stage");
    ASSERT_TRUE(stage_field.has_value());
    auto stage_obj = stage_field->as_object();
    ASSERT_TRUE(stage_obj);
    EXPECT_EQ(
        (*stage_obj)[std::string_view("valueType")].as_sint().value_or(-1),
        4); // select → string
    EXPECT_EQ(
        (*stage_obj)[std::string_view("inputType")].as_sint().value_or(-1),
        1);

    auto retry_field = FindRootField(document, "input.retry");
    ASSERT_TRUE(retry_field.has_value());
    auto retry_obj = retry_field->as_object();
    ASSERT_TRUE(retry_obj);
    EXPECT_EQ(
        (*retry_obj)[std::string_view("valueType")].as_sint().value_or(-1),
        0); // input pipeline_type=int → int
    EXPECT_EQ(
        (*retry_obj)[std::string_view("inputType")].as_sint().value_or(-1),
        0);

    auto medicine_field = FindRootField(document, "input.use-medicine");
    ASSERT_TRUE(medicine_field.has_value());
    auto medicine_obj = medicine_field->as_object();
    ASSERT_TRUE(medicine_obj);
    EXPECT_EQ(
        (*medicine_obj)[std::string_view("valueType")].as_sint().value_or(-1),
        32); // checkbox → array<string>
    EXPECT_EQ(
        (*medicine_obj)[std::string_view("inputType")].as_sint().value_or(-1),
        2);

    auto notify_field = FindRootField(document, "input.notify");
    ASSERT_TRUE(notify_field.has_value());
    auto notify_obj = notify_field->as_object();
    ASSERT_TRUE(notify_obj);
    EXPECT_EQ(
        (*notify_obj)[std::string_view("valueType")].as_sint().value_or(-1),
        8); // switch → bool
    EXPECT_EQ(
        (*notify_obj)[std::string_view("inputType")].as_sint().value_or(-1),
        0);

    // 值默认链：select 无存储回退首 case；input 无存储回退 PI default（int
    // 3）；switch 无存储回退 true（MaaPI 约定）。
    auto stage_value = FieldValueOf(document, "input.stage");
    ASSERT_TRUE(stage_value.is_string());
    EXPECT_EQ(stage_value.as_string().value_or(""), "1-1");
    auto retry_value = FieldValueOf(document, "input.retry");
    ASSERT_TRUE(retry_value.is_sint() || retry_value.is_uint());
    EXPECT_EQ(retry_value.as_sint().value_or(-1), 3);
    auto notify_value = FieldValueOf(document, "input.notify");
    ASSERT_TRUE(notify_value.is_bool());
    EXPECT_TRUE(notify_value.as_bool().value_or(false));
}

// D-A3（111-03）：input.<optionKey> 静态编辑路由——setValue 持久化到首个
// PI task 的 options 存储（与 formSequence item 的 options.* 同一存储），
// 重投影反映新值；未声明 option fail-closed（error 诊断，不落错存储）。
TEST(MaapiProjectionTest, ApplyDocumentSetValueOnPiInputFieldPersistsToTask)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakePiInputDomainMockCatalog();
    AcceptedSettingsDto settings = MakePiInputDomainSettings();

    {
        std::vector<PiDiagnosticDto> diagnostics;
        auto       payload = Das::Utils::ParseYyjsonFromString(
                                 R"({"target":{"kind":"document"},)"
                                 R"("fieldName":"input.stage","value":"1-1"})")
                                 .value_or(yyjson::value(nullptr));
        const bool ok =
            ApplyStrictSetValueChange(settings, payload, &catalog, diagnostics);
        EXPECT_TRUE(ok);
        EXPECT_TRUE(diagnostics.empty());
    }

    // 持久化路径：tasks[0].options。
    ASSERT_EQ(settings.pi.tasks.size(), 1u);
    const auto& task_options = settings.pi.tasks.front().options;
    auto        it = std::find_if(
        task_options.begin(),
        task_options.end(),
        [&](const MaapiPiOptionSettingsDto& o)
        { return o.option_name == "stage"; });
    ASSERT_NE(it, task_options.end());
    ASSERT_EQ(it->selected_cases.size(), 1u);
    EXPECT_EQ(it->selected_cases[0], "1-1");

    // 重投影：input.stage value 反映用户选择（静态编辑闭环）。
    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto value = FieldValueOf(document, "input.stage");
    ASSERT_TRUE(value.is_string());
    EXPECT_EQ(value.as_string().value_or(""), "1-1");

    // fail-closed：首个 task 未声明的 option 键 → error 诊断 + 拒绝。
    std::vector<PiDiagnosticDto> diagnostics;
    auto       bad_payload = Das::Utils::ParseYyjsonFromString(
                                 R"({"target":{"kind":"document"},)"
                                 R"("fieldName":"input.not-declared","value":"x"})")
                                 .value_or(yyjson::value(nullptr));
    const bool bad_ok =
        ApplyStrictSetValueChange(settings, bad_payload, &catalog, diagnostics);
    EXPECT_FALSE(bad_ok);
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics.front().severity, "error");
}

// ============================================================================
// 暴露开关伴生 bool 字段行（componentContext 语境门控）。
//
// 链路：UI 从图节点下钻时请求体携带组件语境标记 → 管线透传进 CreateSession
// 的 context_json → 工厂读 "componentContext"（engaged-only）→ session 成员
// → 投影器门控。standalone（任务编排直开，无标记）与改造前现状逐字节一致
// ——暴露状态仅 component 语境可写。
// ============================================================================
namespace
{
    // fieldLayout 展平：按 section 顺序收集叶子 fieldName（row 内叶子按 row
    // 内顺序展开；commandId 叶子跳过）。fields[] 声明序即 layout 行序，伴生
    // 字段在目标字段紧邻上方 → 展平序列中 index(伴生)+1 == index(目标)。
    std::vector<std::string> FlattenLayoutFieldNames(
        const yyjson::value& document)
    {
        std::vector<std::string> names;
        auto                     doc_obj = document.as_object();
        if (!doc_obj || !doc_obj->contains(std::string_view("fieldLayout")))
        {
            return names;
        }
        auto layout = (*doc_obj)[std::string_view("fieldLayout")].as_array();
        if (!layout)
        {
            return names;
        }
        for (const auto& section : *layout)
        {
            auto section_obj = section.as_object();
            if (!section_obj
                || !section_obj->contains(std::string_view("entries")))
            {
                continue;
            }
            auto entries =
                (*section_obj)[std::string_view("entries")].as_array();
            if (!entries)
            {
                continue;
            }
            for (const auto& entry : *entries)
            {
                auto entry_obj = entry.as_object();
                if (!entry_obj)
                {
                    continue;
                }
                if (entry_obj->contains(std::string_view("fieldName")))
                {
                    names.emplace_back(
                        (*entry_obj)[std::string_view("fieldName")]
                            .as_string()
                            .value_or(""));
                    continue;
                }
                if (entry_obj->contains(std::string_view("row")))
                {
                    auto row = (*entry_obj)[std::string_view("row")].as_array();
                    if (!row)
                    {
                        continue;
                    }
                    for (const auto& leaf : *row)
                    {
                        auto leaf_obj = leaf.as_object();
                        if (leaf_obj
                            && leaf_obj->contains(
                                std::string_view("fieldName")))
                        {
                            names.emplace_back(
                                (*leaf_obj)[std::string_view("fieldName")]
                                    .as_string()
                                    .value_or(""));
                        }
                    }
                }
            }
        }
        return names;
    }
} // namespace

// standalone 钉：无标记 context 的投影与伴生字段机制落地前（基线采集自
// 改造前树）逐字节一致——含 fields/fieldLayout/formSequence/actionCatalog/
// commands 全 section。任何伴生字段/ports section 泄漏进 standalone 都会
// 在此转红。
TEST(MaapiProjectionTest, StandaloneProjectionMatchesPreChangeBaseline)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakePiInputDomainMockCatalog();
    AcceptedSettingsDto settings = MakePiInputDomainSettings();

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto actual = Das::Utils::SerializeYyjsonValue(document, /*pretty=*/true)
                      .value_or("");
    // 基线在伴生字段机制落地前于同一 mock 上采集（en 缺省 locale 的确定
    // 性序列化）。本用例改造前后都必须绿——它是「standalone 零变化」的
    // 字节级证明。
    const std::string baseline = R"maapi({
  "contractVersion": 1,
  "viewKind": "formSequence",
  "revision": 0,
  "fields": [
    {
      "name": "adapter.interfacePath",
      "label": "ProjectInterface path",
      "valueType": 4,
      "inputType": 0,
      "value": ""
    },
    {
      "name": "adapter.executionPolicy.failFast",
      "label": "Fail fast",
      "valueType": 8,
      "inputType": 0,
      "value": true
    },
    {
      "name": "controllerName",
      "label": "控制器",
      "valueType": 4,
      "inputType": 1,
      "value": "",
      "candidates": []
    },
    {
      "name": "resourceName",
      "label": "资源",
      "valueType": 4,
      "inputType": 1,
      "value": "",
      "candidates": []
    },
    {
      "name": "presetName",
      "label": "预设",
      "valueType": 4,
      "inputType": 1,
      "value": "",
      "candidates": []
    },
    {
      "name": "language",
      "label": "语言",
      "valueType": 4,
      "inputType": 1,
      "value": "",
      "candidates": []
    },
    {
      "name": "schedule.kind",
      "label": "Schedule kind",
      "valueType": 4,
      "inputType": 1,
      "value": "",
      "candidates": [
        {
          "value": "off",
          "label": "Off"
        },
        {
          "value": "interval",
          "label": "Interval"
        },
        {
          "value": "daily",
          "label": "Daily"
        }
      ]
    },
    {
      "name": "input.stage",
      "label": "stage",
      "valueType": 4,
      "inputType": 1,
      "value": "1-1",
      "candidates": [
        {
          "value": "1-1",
          "label": "1-1"
        }
      ]
    },
    {
      "name": "input.retry",
      "label": "retry",
      "valueType": 0,
      "inputType": 0,
      "value": 3
    },
    {
      "name": "input.use-medicine",
      "label": "use-medicine",
      "valueType": 32,
      "inputType": 2,
      "value": [],
      "candidates": [
        {
          "value": "1-1",
          "label": "1-1"
        }
      ]
    },
    {
      "name": "input.notify",
      "label": "notify",
      "valueType": 8,
      "inputType": 0,
      "value": true
    }
  ],
  "fieldLayout": [
    {
      "id": "schedule",
      "label": "Schedule",
      "defaultExpanded": true,
      "entries": [
        {
          "fieldName": "schedule.kind"
        }
      ]
    },
    {
      "id": "project",
      "label": "Project",
      "defaultExpanded": true,
      "entries": [
        {
          "fieldName": "adapter.interfacePath"
        },
        {
          "fieldName": "adapter.executionPolicy.failFast"
        },
        {
          "fieldName": "controllerName"
        },
        {
          "fieldName": "resourceName"
        },
        {
          "fieldName": "presetName"
        },
        {
          "fieldName": "language"
        },
        {
          "fieldName": "input.stage"
        },
        {
          "fieldName": "input.retry"
        },
        {
          "fieldName": "input.use-medicine"
        },
        {
          "fieldName": "input.notify"
        }
      ]
    }
  ],
  "formSequence": {
    "items": [
      {
        "id": "item_daily_farm",
        "componentGuid": "69F20008-0000-4000-8000-000000000001",
        "targetKind": "componentRef",
        "label": "DailyFarm",
        "description": "",
        "fields": [
          {
            "name": "options.stage",
            "label": "stage",
            "valueType": 4,
            "inputType": 1,
            "value": "1-1",
            "candidates": [
              {
                "value": "1-1",
                "label": "1-1"
              }
            ]
          },
          {
            "name": "options.retry",
            "label": "retry",
            "valueType": 0,
            "inputType": 0,
            "value": 3
          },
          {
            "name": "options.use-medicine",
            "label": "use-medicine",
            "valueType": 32,
            "inputType": 2,
            "value": [],
            "candidates": [
              {
                "value": "1-1",
                "label": "1-1"
              }
            ]
          },
          {
            "name": "options.notify",
            "label": "notify",
            "valueType": 8,
            "inputType": 0,
            "value": true
          }
        ],
        "enabled": true,
        "debugInfo": {
          "piTaskName": "DailyFarm",
          "piEntry": "StartDaily"
        }
      }
    ]
  },
  "actionCatalog": {
    "entries": [
      {
        "actionId": "DailyFarm",
        "label": "DailyFarm"
      }
    ],
    "groups": []
  },
  "commands": [
    {
      "id": "newPreset",
      "label": "New Preset",
      "apply": {
        "kind": "invokeCommand",
        "payload": {
          "commandId": "newPreset"
        }
      }
    }
  ]
})maapi";
    EXPECT_EQ(actual, baseline);
}

// 显式 false == 缺省（默认参数路径与 standalone 显式关闸逐字节一致）。
TEST(MaapiProjectionTest, ComponentContextExplicitFalseEqualsDefault)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakePiInputDomainMockCatalog();
    AcceptedSettingsDto settings = MakePiInputDomainSettings();

    auto without_flag = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto with_false = ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0,
        /*projection_diagnostics=*/nullptr,
        /*device_cache=*/nullptr,
        /*component_context=*/false);
    EXPECT_EQ(
        Das::Utils::SerializeYyjsonValue(without_flag).value_or(""),
        Das::Utils::SerializeYyjsonValue(with_false).value_or(""));
}

// component 钉：带标记投影为每个可暴露字段生成伴生 bool 行
// （input.<optionKey>_exposed，label「作为节点输入」走 host i18n），
// fieldLayout 排目标字段行的紧邻上一行；checkbox（array<string>，无九词
// 映射）不生成伴生。文档整体过 Core 校验器。
TEST(MaapiProjectionTest, ComponentContextProjectsCompanionExposedToggles)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakePiInputDomainMockCatalog();
    AcceptedSettingsDto settings = MakePiInputDomainSettings();

    std::vector<PiDiagnosticDto> diags;
    auto                         document = ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0,
        &diags,
        /*device_cache=*/nullptr,
        /*component_context=*/true);
    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    // select/input(int)/switch 三类可暴露字段各一伴生 bool 行。
    for (const auto* companion :
         {"input.stage_exposed", "input.retry_exposed", "input.notify_exposed"})
    {
        EXPECT_EQ(CountFieldsWithName(document, companion), 1u) << companion;
        auto field = FindRootField(document, companion);
        ASSERT_TRUE(field.has_value()) << companion;
        auto field_obj = field->as_object();
        ASSERT_TRUE(field_obj) << companion;
        EXPECT_EQ(
            (*field_obj)[std::string_view("valueType")].as_sint().value_or(-1),
            8)
            << companion; // bool → UI CheckBox
        EXPECT_EQ(
            (*field_obj)[std::string_view("inputType")].as_sint().value_or(-1),
            0)
            << companion;
        EXPECT_FALSE((*field_obj)[std::string_view("label")]
                         .as_string()
                         .value_or("")
                         .empty())
            << companion;
    }

    // checkbox（array<string>，无九词映射）不暴露——无伴生字段。
    EXPECT_EQ(CountFieldsWithName(document, "input.use-medicine_exposed"), 0u);

    // 邻接：伴生行在目标字段行的紧邻上方（展平序 index(伴生)+1 ==
    // index(目标)）。
    const auto leaves = FlattenLayoutFieldNames(document);
    auto       index_of = [&leaves](const std::string& name) -> int
    {
        auto it = std::find(leaves.begin(), leaves.end(), name);
        return it == leaves.end() ? -1 : static_cast<int>(it - leaves.begin());
    };
    for (const auto& [companion, target] :
         std::vector<std::pair<std::string, std::string>>{
             {"input.stage_exposed", "input.stage"},
             {"input.retry_exposed", "input.retry"},
             {"input.notify_exposed", "input.notify"}})
    {
        const int companion_index = index_of(companion);
        const int target_index = index_of(target);
        ASSERT_GE(companion_index, 0) << companion;
        ASSERT_GE(target_index, 0) << target;
        EXPECT_EQ(companion_index + 1, target_index)
            << companion << " must sit immediately above " << target;
    }

    // standalone（同 settings 无标记）零伴生字段零新 section。
    auto standalone = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    EXPECT_EQ(CountFieldsWithName(standalone, "input.stage_exposed"), 0u);
    EXPECT_EQ(CountFieldsWithName(standalone, "input.retry_exposed"), 0u);
    EXPECT_EQ(CountFieldsWithName(standalone, "input.notify_exposed"), 0u);
    auto standalone_obj = standalone.as_object();
    ASSERT_TRUE(standalone_obj);
    EXPECT_FALSE(standalone_obj->contains(std::string_view("ports")));
    EXPECT_FALSE(standalone_obj->contains(std::string_view("types")));
}

// 暴露状态存储 + ports section 推导：伴生 bool 行的 setValue（component 语境）
// → exposedInputs upsert/erase → 重投影 ports 增删；非布尔值/不可暴露字段/
// standalone 语境写伴生字段一律 fail-closed 拒绝。
TEST(MaapiProjectionTest, ExposedToggleSetPersistsAndDrivesPortsSection)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakePiInputDomainMockCatalog();
    AcceptedSettingsDto settings = MakePiInputDomainSettings();

    const auto apply_set = [&](const char* field,
                               const char* value_json,
                               bool        component_context,
                               bool        expect_ok,
                               const char* context = "")
    {
        std::vector<PiDiagnosticDto> diagnostics;
        const auto                   payload_string =
            std::string(R"({"target":{"kind":"document"},)")
            + "\"fieldName\":\"" + field + "\",\"value\":" + value_json + "}";
        auto       payload = Das::Utils::ParseYyjsonFromString(payload_string)
                                 .value_or(yyjson::value(nullptr));
        const bool ok = ApplyStrictSetValueChange(
            settings,
            payload,
            &catalog,
            diagnostics,
            component_context);
        EXPECT_EQ(ok, expect_ok)
            << field << " " << value_json << " " << context;
        if (!expect_ok)
        {
            ASSERT_FALSE(diagnostics.empty()) << field << " " << context;
            EXPECT_EQ(diagnostics.front().severity, "error")
                << field << " " << context;
        }
    };

    // 勾选 stage → exposedInputs 键写入。
    apply_set(
        "input.stage_exposed",
        "true",
        /*component_context=*/true,
        /*expect_ok=*/true);
    ASSERT_EQ(settings.pi.exposed_inputs.count("stage"), 1u);
    EXPECT_TRUE(settings.pi.exposed_inputs.at("stage"));

    // 勾选 retry → 两个口，type=九词映射（select→string / input int→int）。
    apply_set(
        "input.retry_exposed",
        "true",
        /*component_context=*/true,
        /*expect_ok=*/true);

    const auto project_ports = [&](bool component) -> std::vector<yyjson::value>
    {
        auto document = ProjectAuthoringDocument(
            settings,
            &catalog,
            {},
            0,
            nullptr,
            nullptr,
            component);
        auto doc_obj = document.as_object();
        if (!doc_obj || !doc_obj->contains(std::string_view("ports")))
        {
            return {};
        }
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        if (!ports)
        {
            return {};
        }
        return std::vector<yyjson::value>(ports->begin(), ports->end());
    };

    {
        auto ports = project_ports(/*component=*/true);
        ASSERT_EQ(ports.size(), 2u);
        auto stage_port = ports[0].as_object();
        ASSERT_TRUE(stage_port);
        EXPECT_EQ(
            (*stage_port)[std::string_view("id")].as_string().value_or(""),
            "stage");
        EXPECT_EQ(
            (*stage_port)[std::string_view("type")].as_string().value_or(""),
            "string");
        EXPECT_EQ(
            (*stage_port)[std::string_view("direction")].as_string().value_or(
                ""),
            "input");
        auto retry_port = ports[1].as_object();
        ASSERT_TRUE(retry_port);
        EXPECT_EQ(
            (*retry_port)[std::string_view("id")].as_string().value_or(""),
            "retry");
        EXPECT_EQ(
            (*retry_port)[std::string_view("type")].as_string().value_or(""),
            "int");
        EXPECT_EQ(
            (*retry_port)[std::string_view("direction")].as_string().value_or(
                ""),
            "input");
    }

    // 带 ports 的文档整体过 Core 校验器（id 纪律/词表/方向门全合法）。
    {
        auto document = ProjectAuthoringDocument(
            settings,
            &catalog,
            {},
            0,
            nullptr,
            nullptr,
            /*component_context=*/true);
        auto validate =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                    << " message=" << validate.message;
    }

    // 取消勾选 stage → ports 无该条 + 键删除（非存 false）。
    apply_set(
        "input.stage_exposed",
        "false",
        /*component_context=*/true,
        /*expect_ok=*/true);
    EXPECT_EQ(settings.pi.exposed_inputs.count("stage"), 0u);
    {
        auto ports = project_ports(/*component=*/true);
        ASSERT_EQ(ports.size(), 1u);
        auto port = ports[0].as_object();
        ASSERT_TRUE(port);
        EXPECT_EQ(
            (*port)[std::string_view("id")].as_string().value_or(""),
            "retry");
    }

    // 全部取消 → ports 清空（component 语境 section 保留空数组自述；
    // 见 ComponentContextAlwaysSelfDescribesPortsSection）。
    apply_set(
        "input.retry_exposed",
        "false",
        /*component_context=*/true,
        /*expect_ok=*/true);
    EXPECT_TRUE(project_ports(/*component=*/true).empty());
    EXPECT_TRUE(settings.pi.exposed_inputs.empty());

    // 即便暴露状态有残留，standalone 投影也不带伴生行/不消费暴露状态。
    {
        auto document = ProjectAuthoringDocument(
            settings,
            &catalog,
            {},
            0,
            nullptr,
            nullptr,
            /*component_context=*/false);
        EXPECT_EQ(CountFieldsWithName(document, "input.stage_exposed"), 0u);
    }

    // fail-closed 三态：非布尔值 / 不可暴露字段（checkbox 无九词映射）/
    // standalone 语境写伴生字段（无此字段=不可写）。
    apply_set(
        "input.stage_exposed",
        "\"yes\"",
        /*component_context=*/true,
        /*expect_ok=*/false,
        "non-bool value");
    apply_set(
        "input.use-medicine_exposed",
        "true",
        /*component_context=*/true,
        /*expect_ok=*/false,
        "checkbox option is not exposable");
    apply_set(
        "input.stage_exposed",
        "true",
        /*component_context=*/false,
        /*expect_ok=*/false,
        "standalone context cannot write exposure");
    EXPECT_TRUE(settings.pi.exposed_inputs.empty());
}

// exposedInputs 持久化：序列化 → acceptedProperties.pi.exposedInputs 对象
// → parse 读回状态保留（会话重建路径）。
TEST(MaapiProjectionTest, ExposedInputsPersistThroughAcceptedProperties)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto settings = MakePiInputDomainSettings();
    settings.pi.exposed_inputs["stage"] = true;
    settings.pi.exposed_inputs["retry"] = true;

    auto serialized = SerializeAcceptedSettings(settings);
    {
        auto obj = serialized.as_object();
        ASSERT_TRUE(obj);
        ASSERT_TRUE(obj->contains(std::string_view("pi")));
        auto pi = (*obj)[std::string_view("pi")].as_object();
        ASSERT_TRUE(pi);
        EXPECT_TRUE(pi->contains(std::string_view("exposedInputs")));
        auto exposed = (*pi)[std::string_view("exposedInputs")].as_object();
        ASSERT_TRUE(exposed);
        EXPECT_EQ(exposed->size(), 2u);
    }

    auto reparsed = ParseAcceptedSettings(serialized);
    EXPECT_EQ(reparsed.pi.exposed_inputs.size(), 2u);
    EXPECT_TRUE(reparsed.pi.exposed_inputs.at("stage"));
    EXPECT_TRUE(reparsed.pi.exposed_inputs.at("retry"));

    // 空态 → 键缺席（engaged-only，legacy fixture 零破坏）。
    auto empty = SerializeAcceptedSettings(MakePiInputDomainSettings());
    auto empty_obj = empty.as_object();
    ASSERT_TRUE(empty_obj);
    auto empty_pi = (*empty_obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(empty_pi);
    EXPECT_FALSE(empty_pi->contains(std::string_view("exposedInputs")));
}

// 文档级联通钉：component 语境文档的 ports section JSON 形状与 faces 通道
// （111.1-01 的 nodeFaces/entryFaces context 键）wire 形状字段级一致——faces
// 口条目锁形规则 = {id,type,direction[,label]}，id/type 非空字符串、
// direction ∈ {input,output}、键集外任何键违规；type 必须落在九词端口类型
// 词表内（与 Core 文档校验器 NODE_PORT_VALUE_TYPES lockstep）。满足本形状
// 的文档 ports 可被 SchedulerService 预备 pass 的 faces 提取原样消费
// （提取器仅取四键逐元素校验）——SchedulerService 全链 E2E 在后续 plan
// 收口，此处钉字段级一致性。
TEST(MaapiProjectionTest, PortsSectionShapeMatchesFacesWireContract)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakePiInputDomainMockCatalog();
    AcceptedSettingsDto settings = MakePiInputDomainSettings();
    settings.pi.exposed_inputs["stage"] = true;
    settings.pi.exposed_inputs["retry"] = true;
    settings.pi.exposed_inputs["notify"] = true;
    // checkbox 不可暴露——即便状态里被外部塞入也不产口（fail-safe）。
    settings.pi.exposed_inputs["use-medicine"] = true;
    // 陈旧勾选（未被首个 task 声明）不产口。
    settings.pi.exposed_inputs["not-declared"] = true;

    auto document = ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0,
        nullptr,
        nullptr,
        /*component_context=*/true);
    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj);
    ASSERT_TRUE(doc_obj->contains(std::string_view("ports")));
    auto ports = (*doc_obj)[std::string_view("ports")].as_array();
    ASSERT_TRUE(ports);
    // use-medicine（checkbox 无九词映射）与 not-declared（陈旧）被跳过。
    ASSERT_EQ(ports->size(), 3u);

    // 九词端口类型词表（与 Core 校验器 NODE_PORT_VALUE_TYPES lockstep 镜像）。
    const std::set<std::string> nine_words{
        "bool",
        "int",
        "int64",
        "float",
        "double",
        "string",
        "image",
        "base",
        "component"};

    std::set<std::string> seen_ids;
    for (const auto& port_value : *ports)
    {
        auto port = port_value.as_object();
        ASSERT_TRUE(port);
        // faces 锁形：键集 ⊆ {id,type,direction,label}（三键必含、label
        // 可选）。
        for (const auto& [key, _] : *port)
        {
            (void)_;
            EXPECT_TRUE(
                key == std::string_view("id") || key == std::string_view("type")
                || key == std::string_view("direction")
                || key == std::string_view("label"))
                << "unexpected port key: " << key;
        }
        const auto id = (*port)[std::string_view("id")].as_string();
        ASSERT_TRUE(id.has_value() && !id->empty());
        const auto type = (*port)[std::string_view("type")].as_string();
        ASSERT_TRUE(type.has_value() && !type->empty());
        const auto direction =
            (*port)[std::string_view("direction")].as_string();
        ASSERT_TRUE(direction.has_value());
        EXPECT_TRUE(
            *direction == std::string_view("input")
            || *direction == std::string_view("output"));
        EXPECT_NE(nine_words.find(std::string(*type)), nine_words.end())
            << "port type outside nine-word vocabulary: " << *type;
        EXPECT_TRUE(seen_ids.insert(std::string(*id)).second)
            << "duplicate port id: " << *id;
    }

    // 三类口各就位（select→string / input int→int / switch→bool）。
    EXPECT_EQ(seen_ids.count("stage"), 1u);
    EXPECT_EQ(seen_ids.count("retry"), 1u);
    EXPECT_EQ(seen_ids.count("notify"), 1u);
}

// component 语境「文档自述」契约：引用闭包注入 pass 消费被引条目文档根
// ports 自述口面——section 缺席即具名编译错（ReferenceDocumentMissing）。
// 零暴露输入的条目也必须自述空口面（空数组），否则任何未配置暴露输入的
// MaaPi 条目被图引用时整图编译被拒。standalone 语境行为不变（恒不投
// 影）。
TEST(MaapiProjectionTest, ComponentContextAlwaysSelfDescribesPortsSection)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakePiInputDomainMockCatalog();
    AcceptedSettingsDto settings = MakePiInputDomainSettings();
    // 零暴露输入——修复前 ports section 整节缺省。

    auto document = ProjectAuthoringDocument(
        settings,
        &catalog,
        {},
        0,
        nullptr,
        nullptr,
        /*component_context=*/true);
    {
        auto validate =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                    << " message=" << validate.message;
        auto doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj);
        ASSERT_TRUE(doc_obj->contains(std::string_view("ports")))
            << "component context must self-describe an (empty) root ports "
               "section";
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports);
        EXPECT_TRUE(ports->empty());
    }

    // standalone（同 settings 无标记）恒不投影——行为不变。
    {
        auto standalone = ProjectAuthoringDocument(
            settings,
            &catalog,
            {},
            0,
            nullptr,
            nullptr,
            /*component_context=*/false);
        auto standalone_obj = standalone.as_object();
        ASSERT_TRUE(standalone_obj);
        EXPECT_FALSE(standalone_obj->contains(std::string_view("ports")));
    }
}

// 会话级钉：context 携带 pi.exposedInputs 重建会话 → 伴生 bool 行勾选态
// + ports section 推导现算可见（状态持久于 acceptedProperties，纯投影无
// 双写）。Apply 响应 document 同样携带 ports（严格 envelope 形状不变）。
TEST_F(MaapiAuthoringFixture, ExposedInputsFromContextDriveSessionDocument)
{
    auto interface_path =
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    const std::string context_json =
        "{\"componentContext\":true,\"adapter\":{\"interfacePath\":\""
        + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_daily_farm\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}],"
          "\"exposedInputs\":{\"stage\":true}}}";
    auto session = CreateSession(context_json);

    // Get：伴生行勾选态 + ports section。
    {
        DasPtr<Das::ExportInterface::IDasJson> doc_json;
        ASSERT_EQ(session->GetDocument(nullptr, doc_json.Put()), DAS_S_OK);
        auto document = DocumentOfGet(doc_json.Get());
        auto doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj);
        auto stage = FindRootField(document, "input.stage_exposed");
        ASSERT_TRUE(stage.has_value());
        auto stage_obj = stage->as_object();
        ASSERT_TRUE(stage_obj);
        EXPECT_TRUE(
            (*stage_obj)[std::string_view("value")].as_bool().value_or(false));
        ASSERT_TRUE(doc_obj->contains(std::string_view("ports")));
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports);
        ASSERT_EQ(ports->size(), 1u);
        auto port = (*ports)[0].as_object();
        ASSERT_TRUE(port);
        EXPECT_EQ(
            (*port)[std::string_view("id")].as_string().value_or(""),
            "stage");
        EXPECT_EQ(
            (*port)[std::string_view("type")].as_string().value_or(""),
            "string");
        EXPECT_EQ(
            (*port)[std::string_view("direction")].as_string().value_or(""),
            "input");
    }

    // Apply：取消勾选 → 响应 document 的 ports 无该条（同一文档投影通道；
    // section 保留空数组——component 语境恒自述被调面，见
    // ComponentContextAlwaysSelfDescribesPortsSection）。
    {
        auto change = ParseDasJson(
            "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
            "\"target\":{\"kind\":\"document\"},"
            "\"fieldName\":\"input.stage_exposed\",\"value\":false}}");
        DasPtr<Das::ExportInterface::IDasJson> result_json;
        ASSERT_EQ(
            session->ApplyChange(change.Get(), result_json.Put()),
            DAS_S_OK);
        auto result = ReadJsonInterface(result_json.Get());
        auto result_obj = result.as_object();
        ASSERT_TRUE(result_obj);
        ASSERT_TRUE(result_obj->contains(std::string_view("document")));
        auto document = (*result_obj)[std::string_view("document")].as_object();
        ASSERT_TRUE(document);
        ASSERT_TRUE(document->contains(std::string_view("ports")));
        {
            auto ports = (*document)[std::string_view("ports")].as_array();
            ASSERT_TRUE(ports);
            EXPECT_TRUE(ports->empty());
        }
        // acceptedProperties 持久化：键删除（不存 false）。
        ASSERT_TRUE(
            result_obj->contains(std::string_view("acceptedProperties")));
        auto accepted =
            (*result_obj)[std::string_view("acceptedProperties")].as_object();
        ASSERT_TRUE(accepted);
        auto pi = (*accepted)[std::string_view("pi")].as_object();
        ASSERT_TRUE(pi);
        EXPECT_FALSE(pi->contains(std::string_view("exposedInputs")));
    }
}

// 会话级钉：真实工厂读 context_json 的 "componentContext"（engaged-only，
// 缺省/非布尔一律 false）——带标记的会话文档含伴生 bool 字段行，缺省 context
// 的会话文档与现状一致（无伴生无 ports/types section）。
TEST_F(MaapiAuthoringFixture, ComponentContextFlagGatesSessionDocument)
{
    auto interface_path =
        Das::Utils::PathToGenericUtf8(FixturePath("interface_authoring.jsonc"));
    const std::string base_context =
        R"({"adapter":{"interfacePath":")" + interface_path
        + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_daily_farm\","
          "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}";

    // 缺省 context：无 componentContext 键 → standalone 形态。
    {
        auto          session = CreateSession(base_context);
        yyjson::value document;
        {
            DasPtr<Das::ExportInterface::IDasJson> doc_json;
            ASSERT_EQ(session->GetDocument(nullptr, doc_json.Put()), DAS_S_OK);
            document = DocumentOfGet(doc_json.Get());
        }
        auto doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj);
        auto fields = (*doc_obj)[std::string_view("fields")].as_array();
        ASSERT_TRUE(fields);
        for (const auto& field : *fields)
        {
            auto field_obj = field.as_object();
            ASSERT_TRUE(field_obj);
            const auto name =
                (*field_obj)[std::string_view("name")].as_string().value_or("");
            EXPECT_FALSE(name.ends_with("_exposed")) << name;
        }
        EXPECT_FALSE(doc_obj->contains(std::string_view("ports")));
        EXPECT_FALSE(doc_obj->contains(std::string_view("types")));
    }

    // 带标记 context：文档含伴生 bool 字段行。
    {
        const std::string component_context_json =
            "{\"componentContext\":true,\"adapter\":{\"interfacePath\":\""
            + interface_path
            + "\"},\"pi\":{\"tasks\":[{\"itemId\":\"item_daily_farm\","
              "\"taskName\":\"DailyFarm\",\"enabled\":true}]}}";
        auto          session = CreateSession(component_context_json);
        yyjson::value document;
        {
            DasPtr<Das::ExportInterface::IDasJson> doc_json;
            ASSERT_EQ(session->GetDocument(nullptr, doc_json.Put()), DAS_S_OK);
            document = DocumentOfGet(doc_json.Get());
        }
        size_t companion_count = 0;
        auto   doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj);
        auto fields = (*doc_obj)[std::string_view("fields")].as_array();
        ASSERT_TRUE(fields);
        for (const auto& field : *fields)
        {
            auto field_obj = field.as_object();
            ASSERT_TRUE(field_obj);
            const auto name =
                (*field_obj)[std::string_view("name")].as_string().value_or("");
            if (name.ends_with("_exposed") && name.starts_with("input."))
            {
                ++companion_count;
            }
        }
        EXPECT_GE(companion_count, 1u);
    }
}

// Round-trip 持久化 RED: settings.pi.pretasks 序列化→parse→project 必须
// 幂等（pretask.<key> value 跨 SerializeAcceptedSettings /
// ParseAcceptedSettings 保持一致）。
TEST(MaapiProjectionTest, PretaskSettingsRoundTripPreservesSelectedValue)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                      catalog = MakeNeutralProjectionMockCatalog();
    AcceptedSettingsDto       settings;
    MaapiPiPretaskSettingsDto pretask_settings;
    pretask_settings.pretask_id = "Bootstrap";
    MaapiPiOptionSettingsDto bootstrap_value;
    bootstrap_value.option_name = "bootstrap_mode";
    bootstrap_value.kind = "select";
    bootstrap_value.selected_cases.emplace_back("fast");
    pretask_settings.options.push_back(std::move(bootstrap_value));
    settings.pi.pretasks.push_back(std::move(pretask_settings));

    auto serialized = SerializeAcceptedSettings(settings);
    auto reparsed = ParseAcceptedSettings(serialized);

    auto document = ProjectAuthoringDocument(reparsed, &catalog, {}, 0);
    EXPECT_EQ(CountFieldsWithName(document, "pretask.bootstrap_mode"), 1u);
    auto value = FieldValueOf(document, "pretask.bootstrap_mode");
    ASSERT_TRUE(value.is_string());
    EXPECT_EQ(value.as_string().value_or(""), "fast");
}

// ============================================================================
// Phase 89 Plan 03 — checkUpdate / applyUpdate invokeCommand dispatch +
// 只读版本字段投影（D-89-04 / D-89-05 / D-89-07 修订）。
//
// 会话级直接测试（不经 IPC）：MaapiAuthoringSession 是 final 具体类，且
// DasMaaPiTest 直接编译本类 + PiUpdateService + FakeUpdateHttpClient seam，故可
// 在进程内构造 session、注入网络 seam、读检查结果表，端到端验证
// checkUpdate → UpdateCheckStore → commands 投影 disabled → applyUpdate
// 全链路。
// ============================================================================
namespace
{
    // D-89-03: 最小网络 seam（同 PiUpdateServiceTest::FakePiHttpClient）。按
    // URL 子串路由 canned 响应，零外网；记录调用供断言路由。
    class FakeUpdateHttpClient final : public PiHttpClient
    {
    public:
        struct Route
        {
            std::string path_fragment;
            std::string body;
            int         status = 200;
        };

        void AddRoute(
            std::string path_fragment,
            std::string body,
            int         status = 200)
        {
            routes_.push_back(
                Route{std::move(path_fragment), std::move(body), status});
        }

        void SetNetworkFailure(bool fail) { fail_all_ = fail; }

        // 下载挂起 latch：BlockDownloads 后 DownloadStream 在命中路由时阻塞，
        // 直到 ReleaseDownload（测试主动）或 RequestCancel（registry 析构收
        // 尾调用——不覆盖则测试进程退出时 join 永等挂死）。等待有界（超时
        // 自行放行），防误用导致的无限挂起。
        void BlockDownloads() { block_downloads_ = true; }

        void ReleaseDownload()
        {
            {
                const std::lock_guard<std::mutex> lock(latch_mutex_);
                release_latch_ = true;
            }
            latch_cv_.notify_all();
        }

        // 检查挂起 latch（checkUpdate 走 Get/Head → Dispatch
        // 路径）：BlockChecks 后 Dispatch 在命中路由时阻塞，直到
        // ReleaseChecks（测试主动）或 RequestCancel（registry 析构收尾同时释放
        // download 与 check 两类 latch——check 挂起用例退出时不双释放则 join
        // 永等挂死）。等待有界 （超时自行放行），防误用导致的无限挂起。
        void BlockChecks() { block_checks_ = true; }

        void ReleaseChecks()
        {
            {
                const std::lock_guard<std::mutex> lock(latch_mutex_);
                release_checks_ = true;
            }
            latch_cv_.notify_all();
        }

        void RequestCancel() override
        {
            ReleaseDownload();
            ReleaseChecks();
        }

        int CountRequestsWith(std::string_view fragment) const
        {
            int count = 0;
            for (const auto& call : calls_)
            {
                if (call.url.find(fragment) != std::string::npos)
                {
                    ++count;
                }
            }
            return count;
        }

        DasResult Get(
            const std::string& url,
            std::string&       out_body,
            int&               out_status,
            const std::string& proxy_url) override
        {
            return Dispatch(url, out_body, out_status, proxy_url);
        }

        DasResult Head(
            const std::string& url,
            int&               out_status,
            const std::string& proxy_url) override
        {
            std::string body;
            return Dispatch(url, body, out_status, proxy_url);
        }

        // 流式下载 mock：命中路由时把 body 写入 out_path（供 ApplyUpdate
        // 流式落盘后解压），成功后回调一次 on_progress。BlockDownloads 置位
        // 时在写盘前阻塞（模拟分钟级下载挂起，驱动受理异步化断言）。
        DasResult DownloadStream(
            const std::string&           url,
            const std::filesystem::path& out_path,
            const ProgressCallback&      on_progress,
            int&                         out_status,
            const std::string&           proxy_url) override
        {
            if (fail_all_)
            {
                return DAS_E_FAIL;
            }
            for (const auto& route : routes_)
            {
                if (url.find(route.path_fragment) != std::string::npos)
                {
                    if (block_downloads_)
                    {
                        std::unique_lock<std::mutex> lock(latch_mutex_);
                        latch_cv_.wait_for(
                            lock,
                            std::chrono::seconds(3),
                            [this] { return release_latch_; });
                    }
                    out_status = route.status;
                    if (route.status != 200)
                    {
                        return DAS_S_OK;
                    }
                    std::ofstream ofs(
                        out_path,
                        std::ios::binary | std::ios::trunc);
                    if (!ofs)
                    {
                        return DAS_E_FAIL;
                    }
                    ofs.write(route.body.data(), route.body.size());
                    if (on_progress)
                    {
                        on_progress(
                            DownloadProgress{
                                static_cast<std::uint64_t>(route.body.size()),
                                static_cast<std::uint64_t>(route.body.size())});
                    }
                    return DAS_S_OK;
                }
            }
            out_status = 404;
            return DAS_S_OK;
        }

    private:
        DasResult Dispatch(
            const std::string& url,
            std::string&       out_body,
            int&               out_status,
            const std::string& proxy_url)
        {
            calls_.push_back(Call{url, proxy_url});
            if (fail_all_)
            {
                return DAS_E_FAIL;
            }
            for (const auto& route : routes_)
            {
                if (url.find(route.path_fragment) != std::string::npos)
                {
                    // BlockChecks 置位时在返回 canned 响应前阻塞（模拟外网
                    // 检查请求挂起，驱动 checkUpdate 受理秒回断言）。
                    if (block_checks_)
                    {
                        std::unique_lock<std::mutex> lock(latch_mutex_);
                        latch_cv_.wait_for(
                            lock,
                            std::chrono::seconds(3),
                            [this] { return release_checks_; });
                    }
                    out_body = route.body;
                    out_status = route.status;
                    return DAS_S_OK;
                }
            }
            out_status = 404;
            return DAS_S_OK;
        }

        struct Call
        {
            std::string url;
            std::string proxy_url;
        };

        std::vector<Route>      routes_;
        std::vector<Call>       calls_;
        bool                    fail_all_ = false;
        bool                    block_downloads_ = false;
        bool                    block_checks_ = false;
        std::mutex              latch_mutex_;
        std::condition_variable latch_cv_;
        bool                    release_latch_ = false;
        bool                    release_checks_ = false;
    };

    // 临时项目目录（interface.json 所在目录 = D-89-01 更新目标）。唯一名防并行
    // ctest 进程（及其 _backup_ / .update 兄弟目录）碰撞，析构清理。
    class TempUpdateProject
    {
    public:
        explicit TempUpdateProject(std::string name = {})
        {
            static int counter{0};
            if (name.empty())
            {
                name = "MaapiUpdate_" + std::to_string(counter++);
            }
            name_ = std::move(name);
            dir_ = std::filesystem::temp_directory_path()
                   / Das::Utils::PathFromUtf8(name_);
            Cleanup();
            std::error_code ec;
            std::filesystem::create_directories(dir_, ec);
        }

        ~TempUpdateProject() { Cleanup(); }

        const std::filesystem::path& Dir() const { return dir_; }

        void WriteInterface(const std::string& json)
        {
            std::ofstream file(
                dir_ / DAS_PATH_LITERAL("interface.json"),
                std::ios::binary);
            file << json;
        }

    private:
        void Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir_, ec);
            // 清理兄弟目录：{name}_backup_{version} + {name}.update。
            const auto parent = dir_.parent_path();
            const auto backup_prefix = name_ + "_backup_";
            const auto update_name = name_ + ".update";
            for (const auto& entry : std::filesystem::directory_iterator(
                     parent,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec))
            {
                const auto leaf =
                    Das::Utils::PathToUtf8(entry.path().filename());
                if (leaf.rfind(backup_prefix, 0) == 0 || leaf == update_name)
                {
                    std::filesystem::remove_all(entry.path(), ec);
                }
            }
        }

        std::string           name_;
        std::filesystem::path dir_;
    };

    // RAII：挂起下载用例的任意退出路径（断言失败/异常逃逸）都先释放 latch
    // 并等待 slot idle——否则 fake（测试侧持有）先于 job 析构，进程退出时
    // registry 收尾的 RequestCancel 悬垂。正常路径的显式释放与之幂等。
    class BlockedDownloadGuard
    {
    public:
        BlockedDownloadGuard(
            std::shared_ptr<FakeUpdateHttpClient> http,
            std::filesystem::path                 pi_dir)
            : http_(std::move(http)), pi_dir_(std::move(pi_dir))
        {
        }

        ~BlockedDownloadGuard()
        {
            http_->ReleaseDownload();
            (void)UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
                pi_dir_,
                std::chrono::seconds(10));
        }

        BlockedDownloadGuard(const BlockedDownloadGuard&) = delete;
        BlockedDownloadGuard& operator=(const BlockedDownloadGuard&) = delete;

    private:
        std::shared_ptr<FakeUpdateHttpClient> http_;
        std::filesystem::path                 pi_dir_;
    };

    // RAII：挂起检查用例的任意退出路径（断言失败/异常逃逸）都先释放 check
    // latch 并等待 slot idle——与 BlockedDownloadGuard 同范式（否则 fake（测试
    // 侧持有）先于 job 析构，进程退出时 registry 收尾的 RequestCancel 悬垂）。
    // 正常路径的显式释放与之幂等。
    class BlockedCheckGuard
    {
    public:
        BlockedCheckGuard(
            std::shared_ptr<FakeUpdateHttpClient> http,
            std::filesystem::path                 pi_dir)
            : http_(std::move(http)), pi_dir_(std::move(pi_dir))
        {
        }

        ~BlockedCheckGuard()
        {
            http_->ReleaseChecks();
            (void)UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
                pi_dir_,
                std::chrono::seconds(10));
        }

        BlockedCheckGuard(const BlockedCheckGuard&) = delete;
        BlockedCheckGuard& operator=(const BlockedCheckGuard&) = delete;

    private:
        std::shared_ptr<FakeUpdateHttpClient> http_;
        std::filesystem::path                 pi_dir_;
    };

    // interface.json 顶层 github 字段 → GitHub 检查来源（MirrorChyan 兜底）。
    std::string MakeUpdateInterface(
        const std::string& version,
        const std::string& github,
        const std::string& name = "test-project")
    {
        return R"({
            "interface_version": 2,
            "name": ")"
               + name + R"(",
            "version": ")"
               + version + R"(",
            "github": ")"
               + github + R"("
        })";
    }

    // GitHub releases/latest API 响应（tag + 可匹配 os=windows arch=x86_64 的
    // asset，d 89-02 MatchGithubAsset 语义）。
    std::string UpdateGithubRelease(const std::string& tag)
    {
        return R"({"tag_name":")" + tag
               + R"(","body":"notes","assets":[{"name":"test-project-win-x86_64-)"
               + tag
               + R"(.zip","size":123,"browser_download_url":"https://github.com/owner/repo/releases/download/)"
               + tag + R"(/test-project-win-x86_64-)" + tag + R"(.zip"}]})";
    }

    // 构造会话直接测试的 AcceptedSettingsDto（adapter.interfacePath →
    // 临时项目目录的 interface.json；generic 形态用 '/' 避免 JSON 转义）。
    AcceptedSettingsDto MakeUpdateSettings(const std::filesystem::path& pi_dir)
    {
        const auto context = Das::Utils::ParseYyjsonFromString(
            R"({"adapter":{"interfacePath":")"
            + Das::Utils::PathToGenericUtf8(
                pi_dir / DAS_PATH_LITERAL("interface.json"))
            + R"("}})");
        return ParseAcceptedSettings(
            context.has_value() ? *context : Das::Utils::MakeYyjsonObject());
    }

    // 种子：向检查结果表写 FoundUpdate 条目（applyUpdate 用例的「已查到
    // 新版」前置——原 session.SetPendingUpdate 种子的 store 化迁移，公开
    // 接口直用）。
    void SeedFoundUpdateOutcome(
        const std::filesystem::path& pi_dir,
        const PiUpdateInfo&          info)
    {
        UpdateCheckEntry entry;
        entry.state = UpdateCheckEntry::State::FoundUpdate;
        entry.info = info;
        entry.info.has_update = true;
        entry.progress_key = info.progress_key;
        UpdateCheckStore::Instance().PutOutcome(pi_dir, std::move(entry));
    }

    struct TestZipEntry
    {
        std::string filename;
        std::string content;
    };

    // 最小 stored ZIP（CRC 用 zlib crc32 逐条目真实计算）。字节布局复刻
    // PiUpdateServiceTest::BuildTestZip 的 stored 分支，供 applyUpdate
    // 下载替换。
    std::string BuildStoredZip(const std::vector<TestZipEntry>& entries)
    {
        auto append_u16 = [](std::string& out, std::uint16_t v)
        {
            out.push_back(static_cast<char>(v & 0xFF));
            out.push_back(static_cast<char>((v >> 8) & 0xFF));
        };
        auto append_u32 = [](std::string& out, std::uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
            {
                out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
            }
        };

        std::string local_section;
        std::string central_section;
        for (const auto& entry : entries)
        {
            const std::uint32_t crc = crc32(
                0,
                reinterpret_cast<const Bytef*>(entry.content.data()),
                static_cast<uInt>(entry.content.size()));
            const auto local_header_offset =
                static_cast<std::uint32_t>(local_section.size());
            append_u32(local_section, 0x04034B50);
            append_u16(local_section, 20);
            append_u16(local_section, 0);
            append_u16(local_section, 0); // stored
            append_u16(local_section, 0);
            append_u16(local_section, 0);
            append_u32(local_section, crc);
            append_u32(
                local_section,
                static_cast<std::uint32_t>(entry.content.size()));
            append_u32(
                local_section,
                static_cast<std::uint32_t>(entry.content.size()));
            append_u16(
                local_section,
                static_cast<std::uint16_t>(entry.filename.size()));
            append_u16(local_section, 0);
            local_section += entry.filename;
            local_section += entry.content;

            append_u32(central_section, 0x02014B50);
            append_u16(central_section, 20);
            append_u16(central_section, 20);
            append_u16(central_section, 0);
            append_u16(central_section, 0);
            append_u16(central_section, 0);
            append_u16(central_section, 0);
            append_u32(central_section, crc);
            append_u32(
                central_section,
                static_cast<std::uint32_t>(entry.content.size()));
            append_u32(
                central_section,
                static_cast<std::uint32_t>(entry.content.size()));
            append_u16(
                central_section,
                static_cast<std::uint16_t>(entry.filename.size()));
            append_u16(central_section, 0);
            append_u16(central_section, 0);
            append_u16(central_section, 0);
            append_u16(central_section, 0);
            append_u32(central_section, 0);
            append_u32(central_section, local_header_offset);
            central_section += entry.filename;
        }

        std::string eocd;
        append_u32(eocd, 0x06054B50);
        append_u16(eocd, 0);
        append_u16(eocd, 0);
        append_u16(eocd, static_cast<std::uint16_t>(entries.size()));
        append_u16(eocd, static_cast<std::uint16_t>(entries.size()));
        append_u32(eocd, static_cast<std::uint32_t>(central_section.size()));
        append_u32(eocd, static_cast<std::uint32_t>(local_section.size()));
        append_u16(eocd, 0);
        return local_section + central_section + eocd;
    }

    // 在 document.commands[] 中按 id 找命令（89-03 commands 投影断言）。
    std::optional<yyjson::value> FindCommand(
        const yyjson::value& document,
        std::string_view     id)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("commands")))
        {
            return std::nullopt;
        }
        auto commands = (*obj)[std::string_view("commands")].as_array();
        if (!commands)
        {
            return std::nullopt;
        }
        for (auto it = commands->begin(); it != commands->end(); ++it)
        {
            auto cmd = it->as_object();
            if (!cmd)
            {
                continue;
            }
            if ((*cmd)[std::string_view("id")].as_string().value_or("") == id)
            {
                return Das::Utils::CloneYyjsonValue(*it);
            }
        }
        return std::nullopt;
    }

    // 对 owning yyjson::value 按下标取字段（object_ref 才有 operator[]，须先
    // as_object().value()）。仅在键确实存在时使用（缺失 operator[] 抛异常）。
    auto FieldOf(const yyjson::value& value, std::string_view key)
    {
        return value.as_object().value()[key];
    }

    // 命令是否含顶层 disabled 键（enabled 时后端省略该键，前端"缺失=false"；
    // 89-04 契约）。
    bool CommandHasDisabled(const std::optional<yyjson::value>& cmd)
    {
        return cmd.has_value()
               && cmd->as_object().value().contains(
                   std::string_view("disabled"));
    }

    std::string ReadFileText(const std::filesystem::path& path)
    {
        std::ifstream      file(path, std::ios::binary);
        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    // 测试缝：本地 IDasProgressService 实现。记录 AddCallback / RemoveCallback
    // / Report 收到的 entry + 可读回字段（无需真 IPC）。
    class FakeProgressService final
        : public Das::ExportInterface::DasProgressServiceImplBase<
              FakeProgressService>
    {
    public:
        int                                                add_count = 0;
        int                                                remove_count = 0;
        DasPtr<Das::ExportInterface::IDasProgressCallback> callback;
        std::vector<DasPtr<Das::ExportInterface::IDasProgressUpdate>> entries;

        DasResult AddCallback(
            Das::ExportInterface::IDasProgressCallback* cb) override
        {
            callback = cb;
            ++add_count;
            return DAS_S_OK;
        }

        DasResult RemoveCallback(
            Das::ExportInterface::IDasProgressCallback* cb) override
        {
            if (cb != nullptr && cb == callback.Get())
            {
                callback.Reset();
            }
            ++remove_count;
            return DAS_S_OK;
        }

        DasResult Report(
            Das::ExportInterface::IDasProgressUpdate* entry) override
        {
            entries.emplace_back(entry);
            return DAS_S_OK;
        }
    };

    // Apply 响应 envelope 的可选 commandResult（checkUpdate 成功时携带）。
    std::optional<yyjson::value> CommandResultOf(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj || !obj->contains(std::string_view("commandResult")))
        {
            return std::nullopt;
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("commandResult")]);
    }

    // entry 的路由 key。
    std::string EntryKey(Das::ExportInterface::IDasProgressUpdate* entry)
    {
        DasPtr<IDasReadOnlyString> key;
        if (DAS::IsFailed(entry->GetKey(key.Put())) || !key)
        {
            return {};
        }
        const char* raw = nullptr;
        if (DAS::IsFailed(key->GetUtf8(&raw)) || raw == nullptr)
        {
            return {};
        }
        return std::string(raw);
    }

    // entry 字段逐字段读回（name + value 文本），镜像
    // MaapiProgressReporterTest 的同名辅助（终态字段增量断言用）。
    std::pair<std::string, std::string> ReadEntryField(
        Das::ExportInterface::IDasProgressUpdate* entry,
        size_t                                    index)
    {
        DasPtr<IDasReadOnlyString>             name;
        DasPtr<Das::ExportInterface::IDasJson> value;
        EXPECT_EQ(
            entry->GetFieldByIndex(index, name.Put(), value.Put()),
            DAS_S_OK);
        const char* name_raw = nullptr;
        EXPECT_EQ(name->GetUtf8(&name_raw), DAS_S_OK);
        DasPtr<IDasReadOnlyString> text;
        EXPECT_EQ(value->ToString(0, text.Put()), DAS_S_OK);
        const char* text_raw = nullptr;
        EXPECT_EQ(text->GetUtf8(&text_raw), DAS_S_OK);
        return {
            name_raw != nullptr ? std::string(name_raw) : std::string{},
            text_raw != nullptr ? std::string(text_raw) : std::string{}};
    }

    // Apply 响应 diagnostics 数组中是否存在 wire code == 给定值的条目
    // （diagnostic 严格 {severity, message, code?, path?}，code 是整数
    // DasResult）。
    bool HasDiagnosticWithCode(const yyjson::value& result, std::int64_t code)
    {
        auto obj = result.as_object();
        if (!obj || !obj->contains(std::string_view("diagnostics")))
        {
            return false;
        }
        auto arr = (*obj)[std::string_view("diagnostics")].as_array();
        if (!arr)
        {
            return false;
        }
        for (auto it = arr->begin(); it != arr->end(); ++it)
        {
            auto diag = it->as_object();
            if (!diag || !diag->contains(std::string_view("code")))
            {
                continue;
            }
            if ((*diag)[std::string_view("code")].as_sint().value_or(0) == code)
            {
                return true;
            }
        }
        return false;
    }

    // RAII：构造时设进程级 GlobalLocale，析构时复位 en（进程单例，测试间
    // 隔离；与 MaapiProgressReporterTest 的同名 guard 同款）。投影 i18n
    // Translate 经 GlobalLocale 解析；需要精确文案断言（如已是最新/检查失败
    // 的中文模板）时使用，其余用例保持 locale 无关断言。
    class GlobalLocaleGuard
    {
    public:
        explicit GlobalLocaleGuard(const char* locale)
        {
            SetGlobalLocale(locale);
        }

        ~GlobalLocaleGuard() { SetGlobalLocale("en"); }

    private:
        static void SetGlobalLocale(const char* utf8)
        {
            DasPtr<IDasReadOnlyString> locale_name;
            ::CreateIDasReadOnlyStringFromUtf8(utf8, locale_name.Put());
            if (locale_name != nullptr)
            {
                ::DasSetDefaultLocale(locale_name.Get());
            }
        }
    };
} // namespace

// checkUpdate 受理化端到端（受理 → running 拒重 → 终态三段）：挂起检查
// 请求下 ApplyChange 秒回 + commandResult.progressKey + 响应文档五字段
// （updateProgress=正在检查更新…，Checking 态）+ 两命令按钮 disabled（job
// running）；挂起期间二次 checkUpdate → updateBusy 诊断（-10007）+ 无
// commandResult + store 条目仍为第一次的 Checking（回滚生效，无第二次
// Checking 残留）；释放后 job 完成 → FoundUpdate 条目（含条目内 key）+ 三
// 字段终态推送（updateVersion「cur → next」/releaseNote/updateProgress 含
// 目标版本）+ 投影按钮恢复（applyUpdate enabled——FoundUpdate + 有下载
// 链接；checkUpdate enabled——slot idle）。
TEST(MaapiUpdateCommandTest, InvokeCommandCheckUpdateDispatchesAndEnablesApply)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->AddRoute(
        "/repos/owner/repo/releases/latest",
        UpdateGithubRelease("v1.2.0"));
    http->BlockChecks();
    BlockedCheckGuard check_guard(http, project.Dir());

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());
    session.SetProgressServiceForTest(service.Get());

    // —— 受理段：挂起检查下秒回（零网络等待）+ progressKey + 五字段 + 两键
    //    disabled。
    const auto accept_start = std::chrono::steady_clock::now();
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"checkUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    const auto accept_elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - accept_start)
            .count();
    auto r1 = ReadJsonInterface(r1_json.Get());
    EXPECT_EQ(CountDiagnosticsBySeverity(r1, "error"), 0u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(r1), 1);
    EXPECT_LT(accept_elapsed_ms, 2000);

    auto cr = CommandResultOf(r1);
    ASSERT_TRUE(cr.has_value());
    auto cr_obj = cr->as_object();
    ASSERT_TRUE(cr_obj.has_value());
    EXPECT_EQ(
        std::string(
            (*cr_obj)[std::string_view("commandId")].as_string().value_or("")),
        "checkUpdate");
    auto data_obj = (*cr_obj)[std::string_view("data")].as_object();
    ASSERT_TRUE(data_obj.has_value());
    const std::string progress_key = std::string(
        (*data_obj)[std::string_view("progressKey")].as_string().value_or(""));
    EXPECT_FALSE(progress_key.empty());

    // 受理响应文档必含五字段（同线程因果：受理写 Checking 先于投影读表；
    // 检查挂起下条目必为 Checking——版本/日志空、updateProgress 为
    // checkRunning 文案，locale 无关断言取非空）。
    auto document = DocumentFromApplyResult(r1);
    for (const std::string_view field_name :
         {"updateVersion",
          "releaseNote",
          "updateProgress",
          "downloadRate",
          "downloadElapsed"})
    {
        auto field = FindRootField(document, field_name);
        ASSERT_TRUE(field.has_value()) << "missing field " << field_name;
    }
    auto accept_version = FindRootField(document, "updateVersion");
    EXPECT_EQ(
        FieldOf(*accept_version, "value").as_string().value_or("x"),
        std::string{});
    auto accept_progress = FindRootField(document, "updateProgress");
    EXPECT_FALSE(
        FieldOf(*accept_progress, "value").as_string().value_or("").empty());
    // job running：两命令按钮 disabled。
    auto check_cmd = FindCommand(document, "checkUpdate");
    ASSERT_TRUE(check_cmd.has_value());
    EXPECT_TRUE(FieldOf(*check_cmd, "disabled").as_bool().value_or(false));
    auto apply_cmd = FindCommand(document, "applyUpdate");
    ASSERT_TRUE(apply_cmd.has_value());
    EXPECT_TRUE(FieldOf(*apply_cmd, "disabled").as_bool().value_or(false));

    // —— running 拒重段：挂起期间二次 checkUpdate → updateBusy 诊断 + 无
    //    commandResult + store 条目仍为第一次的 Checking（恒等回滚生效）。
    DasPtr<Das::ExportInterface::IDasJson> r2_json;
    auto                                   c2 = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"checkUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c2.Get(), r2_json.Put()), DAS_S_OK);
    auto r2 = ReadJsonInterface(r2_json.Get());
    EXPECT_GE(CountDiagnosticsBySeverity(r2, "error"), 1u);
    EXPECT_TRUE(HasDiagnosticWithCode(
        r2,
        static_cast<std::int64_t>(-10007))); // DAS_E_MAAPI_UPDATE_CHECK_FAILED
    EXPECT_FALSE(CommandResultOf(r2).has_value());
    EXPECT_EQ(DocumentRevisionFromApplyResult(r2), 1);
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::Checking);
        EXPECT_EQ(entry->progress_key, progress_key);
    }

    // —— 终态段：释放 latch → job 完成 → FoundUpdate 条目 + 推送 + 投影。
    http->ReleaseChecks();
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
        EXPECT_TRUE(entry->info.has_update);
        EXPECT_EQ(entry->info.version_name, "v1.2.0");
        // 条目内 key（applyUpdate 的 reporter 用同一 key——apply 与 check
        // 同一流）。
        EXPECT_EQ(entry->info.progress_key, progress_key);
    }
    // 检查链只跑一次（被拒的第二次没有发起请求）。
    EXPECT_EQ(http->CountRequestsWith("/repos/owner/repo/releases/latest"), 1);
    // 终态推送：AddCallback/RemoveCallback 成对（首次受理的 reporter 正常
    // Stop；被拒的第二次在 Start 后随 shared_ptr 析构 RAII Stop——各计 1，
    // 合计 2），全部 entry 同 key，最后 entry 三字段（updateVersion /
    // releaseNote / updateProgress——updateProgress 模板 {0}=目标版本）。
    EXPECT_EQ(fake->add_count, 2);
    EXPECT_EQ(fake->remove_count, 2);
    ASSERT_FALSE(fake->entries.empty());
    for (const auto& entry : fake->entries)
    {
        EXPECT_EQ(EntryKey(entry.Get()), progress_key);
    }
    auto& final_entry = fake->entries.back();
    {
        const auto [name, value] = ReadEntryField(final_entry.Get(), 0);
        EXPECT_EQ(name, "updateVersion");
        EXPECT_NE(value.find("1.0.0"), std::string::npos);
        EXPECT_NE(value.find("v1.2.0"), std::string::npos);
    }
    {
        const auto [name, value] = ReadEntryField(final_entry.Get(), 1);
        EXPECT_EQ(name, "releaseNote");
        EXPECT_NE(value.find("notes"), std::string::npos);
    }
    {
        const auto [name, value] = ReadEntryField(final_entry.Get(), 2);
        EXPECT_EQ(name, "updateProgress");
        EXPECT_NE(value.find("v1.2.0"), std::string::npos);
    }
    // 再投影：applyUpdate enabled（FoundUpdate + 有下载链接；缺失
    // disabled == false），checkUpdate enabled（slot idle）。
    auto idle_document = ProjectAuthoringDocument(
        MakeUpdateSettings(project.Dir()),
        nullptr,
        {},
        0);
    auto idle_check = FindCommand(idle_document, "checkUpdate");
    ASSERT_TRUE(idle_check.has_value());
    EXPECT_FALSE(CommandHasDisabled(idle_check));
    auto idle_apply = FindCommand(idle_document, "applyUpdate");
    ASSERT_TRUE(idle_apply.has_value());
    EXPECT_FALSE(CommandHasDisabled(idle_apply));
}

// checkUpdate 无新版（tag == 当前版本）→ UpToDate 终态：受理响应含五字段
// （顺带修复「已是最新无任何界面反馈」的既有缺陷——updateProgress=已是
// 最新（当前版本））；job 完成后 store 条目 UpToDate（has_update=false）→
// 投影 applyUpdate.disabled==true（"更新"按钮置灰）+ 两字段终态推送
// （updateVersion 空串 + updateProgress 模板 {0}=当前版本）。
TEST(MaapiUpdateCommandTest, InvokeCommandCheckUpdateNoUpdateDisablesApply)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->AddRoute(
        "/repos/owner/repo/releases/latest",
        UpdateGithubRelease("v1.0.0"));

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());
    session.SetProgressServiceForTest(service.Get());

    // —— 受理段：无 error + progressKey + 五字段（Checking 态——同线程因果）。
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"checkUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    auto r1 = ReadJsonInterface(r1_json.Get());
    EXPECT_EQ(CountDiagnosticsBySeverity(r1, "error"), 0u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(r1), 1);
    auto cr = CommandResultOf(r1);
    ASSERT_TRUE(cr.has_value());
    auto data_obj =
        cr->as_object().value()[std::string_view("data")].as_object();
    const std::string progress_key = std::string(
        data_obj.value()[std::string_view("progressKey")].as_string().value_or(
            ""));
    EXPECT_FALSE(progress_key.empty());
    auto document = DocumentFromApplyResult(r1);
    auto accept_progress = FindRootField(document, "updateProgress");
    ASSERT_TRUE(accept_progress.has_value());
    EXPECT_FALSE(
        FieldOf(*accept_progress, "value").as_string().value_or("").empty());

    // —— 终态段：job 完成 → UpToDate 条目 + 投影/推送。
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::UpToDate);
        EXPECT_FALSE(entry->info.has_update);
    }
    // 两字段终态推送：updateVersion 空串 + updateProgress 含当前版本
    // （{0}=受理时捕获的 catalog 版本）。
    ASSERT_FALSE(fake->entries.empty());
    for (const auto& entry : fake->entries)
    {
        EXPECT_EQ(EntryKey(entry.Get()), progress_key);
    }
    auto& final_entry = fake->entries.back();
    {
        const auto [name, value] = ReadEntryField(final_entry.Get(), 0);
        EXPECT_EQ(name, "updateVersion");
        // IDasJson::ToString(0) 返回紧凑 JSON 序列化——空串值是两字符 ""。
        EXPECT_EQ(value, "\"\"");
    }
    {
        const auto [name, value] = ReadEntryField(final_entry.Get(), 1);
        EXPECT_EQ(name, "updateProgress");
        EXPECT_NE(value.find("1.0.0"), std::string::npos);
    }
    // 投影：UpToDate 非 FoundUpdate → applyUpdate disabled；checkUpdate
    // enabled（slot idle）。catalog.version 提供当前版本（UpToDate 的 {0}
    // 绑定）。
    auto catalog = MakeStrictProjectionMockCatalog();
    catalog.version = "1.0.0";
    auto idle_document = ProjectAuthoringDocument(
        MakeUpdateSettings(project.Dir()),
        &catalog,
        {},
        0);
    auto idle_apply = FindCommand(idle_document, "applyUpdate");
    ASSERT_TRUE(idle_apply.has_value());
    EXPECT_TRUE(FieldOf(*idle_apply, "disabled").as_bool().value_or(false));
    auto idle_check = FindCommand(idle_document, "checkUpdate");
    ASSERT_TRUE(idle_check.has_value());
    EXPECT_FALSE(CommandHasDisabled(idle_check));
    // UpToDate 投影：updateVersion 空 + updateProgress 含当前版本。
    auto idle_version = FindRootField(idle_document, "updateVersion");
    ASSERT_TRUE(idle_version.has_value());
    EXPECT_EQ(
        FieldOf(*idle_version, "value").as_string().value_or("x"),
        std::string{});
    auto idle_progress = FindRootField(idle_document, "updateProgress");
    ASSERT_TRUE(idle_progress.has_value());
    EXPECT_NE(
        FieldOf(*idle_progress, "value").as_string().value_or("").find("1.0.0"),
        std::string::npos);
}

// checkUpdate 网络失败（受理成功 + 后台 job 立即失败）：受理端点不感知
// 后台失败（无 error、revision 递增、progressKey 返回）；受理成功的响应
// 文档必含五字段（同线程因果——Checking 条目在投影前已写，job 立即失败
// 覆盖为 Failed 后条目依然在）；job 完成后 store 为 Failed 条目（不清空
// ——apply 按钮照样禁用、版本照样空）+ 两字段失败终态推送。
TEST(MaapiUpdateCommandTest, InvokeCommandCheckUpdateFailureEmitsError)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->SetNetworkFailure(true);

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());
    session.SetProgressServiceForTest(service.Get());

    // —— 受理段：成功（受理端点不感知后台失败）。
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"checkUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    auto r1 = ReadJsonInterface(r1_json.Get());
    EXPECT_EQ(CountDiagnosticsBySeverity(r1, "error"), 0u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(r1), 1);
    auto cr = CommandResultOf(r1);
    ASSERT_TRUE(cr.has_value());
    auto data_obj =
        cr->as_object().value()[std::string_view("data")].as_object();
    const std::string progress_key = std::string(
        data_obj.value()[std::string_view("progressKey")].as_string().value_or(
            ""));
    EXPECT_FALSE(progress_key.empty());

    // —— 同线程因果：受理成功的响应文档必含五字段（fake http 立即失败——
    //    条目被 job 覆盖为 Failed 后依然存在）。
    auto document = DocumentFromApplyResult(r1);
    for (const std::string_view field_name :
         {"updateVersion",
          "releaseNote",
          "updateProgress",
          "downloadRate",
          "downloadElapsed"})
    {
        auto field = FindRootField(document, field_name);
        ASSERT_TRUE(field.has_value()) << "missing field " << field_name;
    }

    // —— 终态段：Failed 条目（apply 按钮 disabled + 版本空）。
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::Failed);
        EXPECT_FALSE(entry->info.has_update);
    }
    // 两字段失败终态推送：updateVersion 空串 + updateProgress 为失败分类
    // 文案（locale 无关断言取非空）。
    ASSERT_FALSE(fake->entries.empty());
    for (const auto& entry : fake->entries)
    {
        EXPECT_EQ(EntryKey(entry.Get()), progress_key);
    }
    {
        const auto [name, value] =
            ReadEntryField(fake->entries.back().Get(), 0);
        EXPECT_EQ(name, "updateVersion");
        EXPECT_EQ(value, "\"\"");
    }
    {
        const auto [name, value] =
            ReadEntryField(fake->entries.back().Get(), 1);
        EXPECT_EQ(name, "updateProgress");
        EXPECT_FALSE(value.empty());
    }
    // Failed 条目投影：applyUpdate disabled、updateVersion 空、
    // updateProgress 非空（失败文案）。
    auto idle_document = ProjectAuthoringDocument(
        MakeUpdateSettings(project.Dir()),
        nullptr,
        {},
        0);
    auto idle_apply = FindCommand(idle_document, "applyUpdate");
    ASSERT_TRUE(idle_apply.has_value());
    EXPECT_TRUE(FieldOf(*idle_apply, "disabled").as_bool().value_or(false));
    auto idle_version = FindRootField(idle_document, "updateVersion");
    ASSERT_TRUE(idle_version.has_value());
    EXPECT_EQ(
        FieldOf(*idle_version, "value").as_string().value_or("x"),
        std::string{});
    auto idle_progress = FindRootField(idle_document, "updateProgress");
    ASSERT_TRUE(idle_progress.has_value());
    EXPECT_FALSE(
        FieldOf(*idle_progress, "value").as_string().value_or("").empty());
}

// checkUpdate 受理后 Apply 响应 envelope 携带 commandResult={commandId,
// data:{progressKey}}（SSE 订阅 key，随机不可枚举）；无业务结果的命令
// （newPreset）不带 commandResult。
TEST(
    MaapiUpdateCommandTest,
    InvokeCommandCheckUpdateReturnsProgressKeyInCommandResult)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->AddRoute(
        "/repos/owner/repo/releases/latest",
        UpdateGithubRelease("v1.2.0"));

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"checkUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    auto cr = CommandResultOf(result);
    ASSERT_TRUE(cr.has_value());
    auto cr_obj = cr->as_object();
    ASSERT_TRUE(cr_obj.has_value());
    EXPECT_EQ(
        std::string(
            (*cr_obj)[std::string_view("commandId")].as_string().value_or("")),
        "checkUpdate");
    auto data_obj = (*cr_obj)[std::string_view("data")].as_object();
    ASSERT_TRUE(data_obj.has_value());
    const std::string progress_key = std::string(
        (*data_obj)[std::string_view("progressKey")].as_string().value_or(""));
    EXPECT_FALSE(progress_key.empty());

    // 其它命令（newPreset）不带 commandResult。
    DasPtr<Das::ExportInterface::IDasJson> r2_json;
    auto                                   c2 = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"newPreset\"}}");
    ASSERT_EQ(session.ApplyChange(c2.Get(), r2_json.Put()), DAS_S_OK);
    auto r2 = ReadJsonInterface(r2_json.Get());
    EXPECT_FALSE(CommandResultOf(r2).has_value());

    // job 完成后条目内 key 与 commandResult 同一（applyUpdate 用同一 key
    // 订阅进度流）。
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
    const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
    EXPECT_EQ(entry->info.progress_key, progress_key);
}

// D-08（未检查无字段）：无 checkUpdate 历史（store 无条目）的文档投影——
// 五字段（updateVersion/releaseNote/updateProgress/downloadRate/
// downloadElapsed）全部不存在；更新 section 常驻字段（CDK/代理/通道/两个
// 版本只读）照常投影；两命令按钮在 commands[]（applyUpdate disabled——
// 无条目即无 FoundUpdate；checkUpdate enabled——slot idle）；fieldLayout
// 更新 section 仍存在但不置顶（无条目时置顶布尔为 false）。
TEST(
    MaapiUpdateCommandTest,
    InvokeCommandUncheckedDocumentProjectsNoUpdateFields)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    // 从未点过检查：store 无该 pi_dir 条目。
    EXPECT_FALSE(UpdateCheckStore::Instance().Get(project.Dir()).has_value());

    auto document = ProjectAuthoringDocument(
        MakeUpdateSettings(project.Dir()),
        nullptr,
        {},
        0);

    // 五字段全部不存在（更新 panel 不常驻，点检查才出现）。
    for (const std::string_view field_name :
         {"updateVersion",
          "releaseNote",
          "updateProgress",
          "downloadRate",
          "downloadElapsed"})
    {
        EXPECT_FALSE(FindRootField(document, field_name).has_value())
            << "unchecked document must not contain " << field_name;
    }

    // 更新 section 常驻字段照常投影。
    EXPECT_TRUE(FindRootField(document, "adapter.mirrorchyanCdk").has_value());
    EXPECT_TRUE(FindRootField(document, "adapter.proxyUrl").has_value());
    EXPECT_TRUE(FindRootField(document, "adapter.updateChannel").has_value());
    EXPECT_TRUE(FindRootField(document, "resourceVersion").has_value());
    EXPECT_TRUE(FindRootField(document, "maafwVersion").has_value());

    // 两命令按钮：applyUpdate disabled（无条目即无 FoundUpdate，按钮不携带
    // 任何更新信息）；checkUpdate enabled（无 job 运行）。
    auto apply_cmd = FindCommand(document, "applyUpdate");
    ASSERT_TRUE(apply_cmd.has_value());
    EXPECT_TRUE(FieldOf(*apply_cmd, "disabled").as_bool().value_or(false));
    auto check_cmd = FindCommand(document, "checkUpdate");
    ASSERT_TRUE(check_cmd.has_value());
    EXPECT_FALSE(CommandHasDisabled(check_cmd));

    // fieldLayout：更新 section 存在（常驻字段仍在）但不置顶（无条目）。
    ASSERT_TRUE(FindLayoutSection(document, "update").has_value());
    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    ASSERT_TRUE(doc_obj->contains(std::string_view("fieldLayout")));
    auto layout = (*doc_obj)[std::string_view("fieldLayout")].as_array();
    ASSERT_TRUE(layout.has_value());
    ASSERT_FALSE(layout->empty());
    auto first_section = layout->begin()->as_object();
    ASSERT_TRUE(first_section.has_value());
    EXPECT_NE(
        std::string(
            (*first_section)[std::string_view("id")].as_string().value_or("")),
        "update");
}

// D-08（Failed 条目覆盖，按钮不得携带陈旧更新信息）：首次检查发现新版 →
// FoundUpdate 条目（apply 按钮 enabled、版本「cur → next」）；再次检查网络
// 失败 → Failed 条目覆盖（不清空表）→ 再投影：apply 按钮 disabled、
// updateVersion 回到空串（陈旧 has_update/download_url 不得残留）、
// updateProgress 为「检查失败：{分类文案}」——复查失败语义与首查失败一致。
TEST(MaapiUpdateCommandTest, InvokeCommandCheckUpdateFailedOverwritesStaleFound)
{
    using namespace Das::Plugins::DasMaaPi;
    GlobalLocaleGuard locale_guard("zh-cn");

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->AddRoute(
        "/repos/owner/repo/releases/latest",
        UpdateGithubRelease("v1.2.0"));

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());

    // —— 首查：发现新版 → FoundUpdate（apply 可点、版本含目标版本）。
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"checkUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
    }
    {
        auto stale_document = ProjectAuthoringDocument(
            MakeUpdateSettings(project.Dir()),
            nullptr,
            {},
            0);
        auto stale_apply = FindCommand(stale_document, "applyUpdate");
        ASSERT_TRUE(stale_apply.has_value());
        EXPECT_FALSE(CommandHasDisabled(stale_apply));
        auto stale_version_field =
            FindRootField(stale_document, "updateVersion");
        ASSERT_TRUE(stale_version_field.has_value());
        EXPECT_NE(
            FieldOf(*stale_version_field, "value")
                .as_string()
                .value_or("")
                .find("v1.2.0"),
            std::string::npos);
    }

    // —— 复查（网络失败）：Failed 条目覆盖 FoundUpdate。
    http->SetNetworkFailure(true);
    DasPtr<Das::ExportInterface::IDasJson> r2_json;
    auto                                   c2 = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"checkUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c2.Get(), r2_json.Put()), DAS_S_OK);
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::Failed);
        EXPECT_FALSE(entry->info.has_update);
    }

    // —— 再投影：apply 按钮 disabled、版本空串（陈旧版本/下载地址不得残留）、
    //    updateProgress 为检查失败文案。
    auto document = ProjectAuthoringDocument(
        MakeUpdateSettings(project.Dir()),
        nullptr,
        {},
        0);
    auto apply_cmd = FindCommand(document, "applyUpdate");
    ASSERT_TRUE(apply_cmd.has_value());
    EXPECT_TRUE(FieldOf(*apply_cmd, "disabled").as_bool().value_or(false));
    auto version_field = FindRootField(document, "updateVersion");
    ASSERT_TRUE(version_field.has_value());
    EXPECT_EQ(
        FieldOf(*version_field, "value").as_string().value_or("x"),
        std::string{});
    auto progress_field = FindRootField(document, "updateProgress");
    ASSERT_TRUE(progress_field.has_value());
    EXPECT_TRUE(FieldOf(*progress_field, "value")
                    .as_string()
                    .value_or("")
                    .starts_with("检查失败："));
}

// D-08（UpToDate 反馈修复）：无更新终态投影五字段全部出现（同步流程下
// 「已是最新时字段不出现、界面零反馈」的既有缺陷修复断言）；
// updateVersion 空串、updateProgress==已是最新（{当前版本}）（{0} 绑定
// 投影时的 catalog 版本）、releaseNote/downloadRate/downloadElapsed 空串。
TEST(MaapiUpdateCommandTest, InvokeCommandCheckUpdateUpToDateProjectionFeedback)
{
    using namespace Das::Plugins::DasMaaPi;
    GlobalLocaleGuard locale_guard("zh-cn");

    TempUpdateProject project;

    UpdateCheckEntry up_to_date;
    up_to_date.state = UpdateCheckEntry::State::UpToDate;
    up_to_date.progress_key = "up-to-date-seed-key";
    UpdateCheckStore::Instance().PutOutcome(
        project.Dir(),
        std::move(up_to_date));

    auto catalog = MakeStrictProjectionMockCatalog();
    catalog.version = "v9.9.9";
    auto document = ProjectAuthoringDocument(
        MakeUpdateSettings(project.Dir()),
        &catalog,
        {},
        0);

    // 五字段全部出现（缺陷修复：UpToDate 也有界面反馈）。
    for (const std::string_view field_name :
         {"updateVersion",
          "releaseNote",
          "updateProgress",
          "downloadRate",
          "downloadElapsed"})
    {
        ASSERT_TRUE(FindRootField(document, field_name).has_value())
            << "missing field " << field_name;
    }
    auto version_field = FindRootField(document, "updateVersion");
    ASSERT_TRUE(version_field.has_value());
    EXPECT_EQ(
        FieldOf(*version_field, "value").as_string().value_or("x"),
        std::string{});
    auto progress_field = FindRootField(document, "updateProgress");
    ASSERT_TRUE(progress_field.has_value());
    EXPECT_EQ(
        FieldOf(*progress_field, "value").as_string().value_or(""),
        "已是最新（v9.9.9）");
    auto release_field = FindRootField(document, "releaseNote");
    ASSERT_TRUE(release_field.has_value());
    EXPECT_EQ(
        FieldOf(*release_field, "value").as_string().value_or("x"),
        std::string{});
    auto rate_field = FindRootField(document, "downloadRate");
    ASSERT_TRUE(rate_field.has_value());
    EXPECT_EQ(
        FieldOf(*rate_field, "value").as_string().value_or("x"),
        std::string{});
    auto elapsed_field = FindRootField(document, "downloadElapsed");
    ASSERT_TRUE(elapsed_field.has_value());
    EXPECT_EQ(
        FieldOf(*elapsed_field, "value").as_string().value_or("x"),
        std::string{});
}

// D-08（置顶顺序）：store 有条目（四态各一变体）时投影 document 的
// fieldLayout 首位是更新 section（四态全置顶的可观察断言）；无条目时不
// 置顶（更新 section 仍存在但不首位）。ResolvePiDir 只取 interface_path
// 的父目录字符串（不触碰文件系统），用唯一假路径隔离 per-test store 键。
TEST(MaapiUpdateCommandTest, UpdateSectionTopOrderingAcrossCheckEntryStates)
{
    using namespace Das::Plugins::DasMaaPi;

    const auto make_seeded_settings = [](const std::string& variant)
    {
        AcceptedSettingsDto settings;
        settings.adapter.interface_path =
            "C:/das-authoring-check-top/" + variant + "/interface.json";
        return settings;
    };
    const auto dir_of = [](const AcceptedSettingsDto& settings)
    {
        return Das::Utils::PathFromUtf8(*settings.adapter.interface_path)
            .parent_path();
    };
    const auto first_section_id = [](const yyjson::value& document)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("fieldLayout")))
        {
            return std::string{};
        }
        auto layout = (*obj)[std::string_view("fieldLayout")].as_array();
        if (!layout || layout->empty())
        {
            return std::string{};
        }
        auto first = layout->begin()->as_object();
        if (!first)
        {
            return std::string{};
        }
        return std::string(
            (*first)[std::string_view("id")].as_string().value_or(""));
    };

    // Checking 变体：受理写入的挂起态也置顶。
    {
        auto settings = make_seeded_settings("checking");
        (void)UpdateCheckStore::Instance().SaveAndPutChecking(
            dir_of(settings),
            "top-checking-key");
        auto document = ProjectAuthoringDocument(settings, nullptr, {}, 0);
        EXPECT_EQ(first_section_id(document), "update");
    }
    // FoundUpdate 变体。
    {
        auto         settings = make_seeded_settings("found-update");
        PiUpdateInfo info;
        info.has_update = true;
        info.version_name = "v1.2.0";
        info.download_url = "https://example.com/pkg.zip";
        info.progress_key = "top-found-key";
        SeedFoundUpdateOutcome(dir_of(settings), info);
        auto document = ProjectAuthoringDocument(settings, nullptr, {}, 0);
        EXPECT_EQ(first_section_id(document), "update");
    }
    // UpToDate 变体。
    {
        auto             settings = make_seeded_settings("up-to-date");
        UpdateCheckEntry up_to_date;
        up_to_date.state = UpdateCheckEntry::State::UpToDate;
        up_to_date.progress_key = "top-uptodate-key";
        UpdateCheckStore::Instance().PutOutcome(
            dir_of(settings),
            std::move(up_to_date));
        auto document = ProjectAuthoringDocument(settings, nullptr, {}, 0);
        EXPECT_EQ(first_section_id(document), "update");
    }
    // Failed 变体。
    {
        auto             settings = make_seeded_settings("failed");
        UpdateCheckEntry failed;
        failed.state = UpdateCheckEntry::State::Failed;
        failed.progress_key = "top-failed-key";
        failed.failure_reason_key = "diagnostic.checkUpdateNetworkFailed";
        UpdateCheckStore::Instance().PutOutcome(
            dir_of(settings),
            std::move(failed));
        auto document = ProjectAuthoringDocument(settings, nullptr, {}, 0);
        EXPECT_EQ(first_section_id(document), "update");
    }
    // 无条目：不置顶（更新 section 仍存在）。
    {
        auto settings = make_seeded_settings("no-entry");
        auto document = ProjectAuthoringDocument(settings, nullptr, {}, 0);
        EXPECT_NE(first_section_id(document), "update");
        EXPECT_TRUE(FindLayoutSection(document, "update").has_value());
    }
}

// applyUpdate 受理后后台 job 经进度 service 上报字段增量：受理返回
// accepted commandResult；等待缝收口 job 后 AddCallback → Report（entry key
// == checkUpdate 返回的 progress_key，含终态 entry）→ RemoveCallback 成对
// （proxy 释放）。
TEST(MaapiUpdateCommandTest, ApplyUpdateReportsProgressWithCheckUpdateKey)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    const std::string new_interface =
        MakeUpdateInterface("1.2.0", "https://github.com/owner/repo");
    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->AddRoute(
        "/repos/owner/repo/releases/latest",
        UpdateGithubRelease("v1.2.0"));
    http->AddRoute(
        "/releases/download/v1.2.0/",
        BuildStoredZip({{"interface.json", new_interface}}),
        200);

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());
    session.SetProgressServiceForTest(service.Get());

    // checkUpdate → progress_key（commandResult 返回）。
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"checkUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    auto r1 = ReadJsonInterface(r1_json.Get());
    auto cr = CommandResultOf(r1);
    ASSERT_TRUE(cr.has_value());
    auto data_obj =
        cr->as_object().value()[std::string_view("data")].as_object();
    ASSERT_TRUE(data_obj.has_value());
    const std::string progress_key = std::string(
        (*data_obj)[std::string_view("progressKey")].as_string().value_or(""));
    EXPECT_FALSE(progress_key.empty());

    // check job 完成落 FoundUpdate 条目（applyUpdate 前置读检查结果表——
    // 受理化后结果是异步落的，须先收口）。
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
    }

    // applyUpdate → 受理（accepted commandResult，无 error）。
    DasPtr<Das::ExportInterface::IDasJson> r2_json;
    auto                                   c2 = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"applyUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c2.Get(), r2_json.Put()), DAS_S_OK);
    auto r2 = ReadJsonInterface(r2_json.Get());
    EXPECT_EQ(CountDiagnosticsBySeverity(r2, "error"), 0u);
    auto apply_cr = CommandResultOf(r2);
    ASSERT_TRUE(apply_cr.has_value());
    EXPECT_EQ(
        std::string(apply_cr->as_object()
                        .value()[std::string_view("commandId")]
                        .as_string()
                        .value_or("")),
        "applyUpdate");

    // 等待缝收口 job → 进度上报（AddCallback + Report + RemoveCallback 成对，
    // entries 含终态）。check 与 apply 两个 job 各起一个 reporter（同一 key）
    // → 计数各 2。
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));

    EXPECT_EQ(fake->add_count, 2);
    EXPECT_EQ(fake->remove_count, 2);
    EXPECT_FALSE(fake->entries.empty());
    for (const auto& entry : fake->entries)
    {
        EXPECT_EQ(EntryKey(entry.Get()), progress_key);
    }
}

// D-89-05（Tracer 3）：无 pending（未先检查/已清）时 applyUpdate 拒绝 → error
// 诊断 + 不递增 revision。
TEST(MaapiUpdateCommandTest, InvokeCommandApplyUpdateRejectsWithoutPending)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));

    DasPtr<Das::ExportInterface::IDasJson> result_json;
    auto                                   change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"applyUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(change.Get(), result_json.Put()), DAS_S_OK);
    auto result = ReadJsonInterface(result_json.Get());

    EXPECT_GE(CountDiagnosticsBySeverity(result, "error"), 1u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(result), 0);
}

// applyUpdate 全链路（异步受理语义）——Fake seam 下载 zip → 解压 → 备份 →
// 原子替换 pi_dir 在后台 job 执行：受理返回 accepted + revision 递增，等待缝
// 收口后文件已替换；受理与 job 完成均不清检查结果条目（清理交给下次
// checkUpdate 覆盖），job 完成后再次 applyUpdate 不被 no-check-result 前置
// 拒绝。
TEST(MaapiUpdateCommandTest, InvokeCommandApplyUpdateAcceptedRetainsPending)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    const std::string new_interface =
        MakeUpdateInterface("1.2.0", "https://github.com/owner/repo");
    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->AddRoute(
        "/releases/download/v1.2.0/",
        BuildStoredZip({{"interface.json", new_interface}}),
        200);

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());
    PiUpdateInfo info;
    info.has_update = true;
    info.version_name = "v1.2.0";
    info.download_url =
        "https://github.com/owner/repo/releases/download/v1.2.0/"
        "test-project-win-x86_64-v1.2.0.zip";
    SeedFoundUpdateOutcome(project.Dir(), info);

    // 1) applyUpdate 受理成功（accepted commandResult，无同步执行）。
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"applyUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    auto r1 = ReadJsonInterface(r1_json.Get());
    EXPECT_EQ(CountDiagnosticsBySeverity(r1, "error"), 0u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(r1), 1);
    auto cr = CommandResultOf(r1);
    ASSERT_TRUE(cr.has_value());
    EXPECT_EQ(
        std::string(cr->as_object()
                        .value()[std::string_view("commandId")]
                        .as_string()
                        .value_or("")),
        "applyUpdate");
    auto accepted = cr->as_object()
                        .value()[std::string_view("data")]
                        .as_object()
                        .value()[std::string_view("accepted")]
                        .as_bool()
                        .value_or(false);
    EXPECT_TRUE(accepted);
    // 受理后检查结果条目保留。
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
    }

    // 2) 等待缝收口 job → pi_dir 已被替换：interface.json 现在是新版（1.2.0）。
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
    const auto replaced =
        ReadFileText(project.Dir() / DAS_PATH_LITERAL("interface.json"));
    EXPECT_NE(replaced.find("1.2.0"), std::string::npos);
    EXPECT_EQ(replaced.find("1.0.0"), std::string::npos);
    // job 完成后检查结果条目仍保留。
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
    }

    // 3) 二次 applyUpdate（pending 保留）→ 不被 has-no-pending 前置拒绝
    // （受理成功，第二次 job 再跑一遍替换）。
    DasPtr<Das::ExportInterface::IDasJson> r2_json;
    auto                                   c2 = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"applyUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c2.Get(), r2_json.Put()), DAS_S_OK);
    auto r2 = ReadJsonInterface(r2_json.Get());
    EXPECT_EQ(CountDiagnosticsBySeverity(r2, "error"), 0u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(r2), 2);
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
}

// 受理异步化端到端：fake 下载挂起（latch）时 applyUpdate 受理在 2 秒内返回，
// accepted commandResult 正确；释放 latch 后 job 完成，终态字段增量经进度
// service 可读（updateProgress 含版本号、downloadRate/downloadElapsed 空串），
// pi_dir 替换完成且 pending 保留。测试进程退出不挂死（fake 覆盖
// RequestCancel 释放 latch + registry 析构 join 有界）。
TEST(
    MaapiUpdateCommandTest,
    InvokeCommandApplyUpdateAcceptedRunsInBackgroundAndReportsFinal)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    const std::string new_interface =
        MakeUpdateInterface("1.2.0", "https://github.com/owner/repo");
    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->AddRoute(
        "/releases/download/v1.2.0/",
        BuildStoredZip({{"interface.json", new_interface}}),
        200);
    http->BlockDownloads();
    BlockedDownloadGuard download_guard(http, project.Dir());

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());
    session.SetProgressServiceForTest(service.Get());

    PiUpdateInfo info;
    info.has_update = true;
    info.version_name = "v1.2.0";
    info.download_url =
        "https://github.com/owner/repo/releases/download/v1.2.0/"
        "test-project-win-x86_64-v1.2.0.zip";
    info.progress_key = "accepted-final-key";
    SeedFoundUpdateOutcome(project.Dir(), info);

    // 受理：下载挂起时 ApplyChange 在 2 秒内返回。
    const auto accept_start = std::chrono::steady_clock::now();
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"applyUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    const auto accept_elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - accept_start)
            .count();
    auto r1 = ReadJsonInterface(r1_json.Get());
    EXPECT_EQ(CountDiagnosticsBySeverity(r1, "error"), 0u);
    EXPECT_LT(accept_elapsed_ms, 2000);

    auto cr = CommandResultOf(r1);
    ASSERT_TRUE(cr.has_value());
    auto cr_obj = cr->as_object();
    ASSERT_TRUE(cr_obj.has_value());
    EXPECT_EQ(
        std::string(
            (*cr_obj)[std::string_view("commandId")].as_string().value_or("")),
        "applyUpdate");
    auto data_obj = (*cr_obj)[std::string_view("data")].as_object();
    ASSERT_TRUE(data_obj.has_value());
    EXPECT_TRUE(
        (*data_obj)[std::string_view("accepted")].as_bool().value_or(false));

    // 释放 latch → job 完成（有界等待）。
    http->ReleaseDownload();
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));

    // 终态 entry：updateProgress 含版本号（i18n 模板 {0} 填充），
    // downloadRate/downloadElapsed 为空串；全部 entry 走同一 key。
    ASSERT_FALSE(fake->entries.empty());
    for (const auto& entry : fake->entries)
    {
        EXPECT_EQ(EntryKey(entry.Get()), "accepted-final-key");
    }
    auto& final_entry = fake->entries.back();
    {
        const auto [name, value] = ReadEntryField(final_entry.Get(), 0);
        EXPECT_EQ(name, "updateProgress");
        EXPECT_NE(value.find("v1.2.0"), std::string::npos);
    }
    {
        const auto [name, value] = ReadEntryField(final_entry.Get(), 1);
        EXPECT_EQ(name, "downloadRate");
        // IDasJson::ToString(0) 返回紧凑 JSON 序列化——空串值是两字符 ""。
        EXPECT_EQ(value, "\"\"");
    }
    {
        const auto [name, value] = ReadEntryField(final_entry.Get(), 2);
        EXPECT_EQ(name, "downloadElapsed");
        EXPECT_EQ(value, "\"\"");
    }

    // pi_dir 已替换 + 检查结果条目保留。
    const auto replaced =
        ReadFileText(project.Dir() / DAS_PATH_LITERAL("interface.json"));
    EXPECT_NE(replaced.find("1.2.0"), std::string::npos);
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
    }
}

// 防重入：job 运行中（下载挂起）再次 applyUpdate → 拒绝（ok=false）+
// error 级诊断（DAS_E_MAAPI_UPDATE_APPLY_FAILED）+ 无 accepted
// commandResult；registry 仍 running。收尾经 fake 的 RequestCancel 释放
// latch（同时覆盖取消覆盖方法本身）+ 等待缝收干净 slot。
TEST(MaapiUpdateCommandTest, InvokeCommandApplyUpdateRejectedWhileJobRunning)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    const std::string new_interface =
        MakeUpdateInterface("1.2.0", "https://github.com/owner/repo");
    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->AddRoute(
        "/releases/download/v1.2.0/",
        BuildStoredZip({{"interface.json", new_interface}}),
        200);
    http->BlockDownloads();
    BlockedDownloadGuard download_guard(http, project.Dir());

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());
    PiUpdateInfo info;
    info.has_update = true;
    info.version_name = "v1.2.0";
    info.download_url =
        "https://github.com/owner/repo/releases/download/v1.2.0/"
        "test-project-win-x86_64-v1.2.0.zip";
    SeedFoundUpdateOutcome(project.Dir(), info);

    // 1) 首次受理成功（job 挂起在下载中）。
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"applyUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    auto r1 = ReadJsonInterface(r1_json.Get());
    EXPECT_EQ(CountDiagnosticsBySeverity(r1, "error"), 0u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(r1), 1);
    ASSERT_TRUE(UpdateJobRegistry::Instance().IsRunning(project.Dir()));

    // 2) job 运行中二次 applyUpdate → 拒绝 + error 诊断（错误码 -10008）+
    //    无 accepted commandResult + revision 不递增。
    DasPtr<Das::ExportInterface::IDasJson> r2_json;
    auto                                   c2 = ParseDasJson(
        "{\"baseRevision\":1,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"applyUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c2.Get(), r2_json.Put()), DAS_S_OK);
    auto r2 = ReadJsonInterface(r2_json.Get());
    EXPECT_GE(CountDiagnosticsBySeverity(r2, "error"), 1u);
    EXPECT_TRUE(HasDiagnosticWithCode(r2, static_cast<std::int64_t>(-10008)));
    EXPECT_FALSE(CommandResultOf(r2).has_value());
    EXPECT_EQ(DocumentRevisionFromApplyResult(r2), 1);
    EXPECT_TRUE(UpdateJobRegistry::Instance().IsRunning(project.Dir()));

    // 3) 收尾：经 fake 的 RequestCancel 释放挂起下载，等待缝收干净 slot。
    http->RequestCancel();
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
    EXPECT_FALSE(UpdateJobRegistry::Instance().IsRunning(project.Dir()));
}

// 受理占位与线程构造在 TryStart 锁内一体完成：不变量 running==true ⇒
// worker.joinable()==true（线程构造不外泄到锁外，析构快照不可能撞上
// 「running 已置而 worker 未赋」的跳 join 窗口）；job 体经参数获得 slot，
// 取消标志置位后 job 体观察到即收尾 Finish。
TEST(MaapiUpdateCommandTest, TryStartConstructsWorkerUnderLockAndHonorsCancel)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    std::atomic<bool> body_entered{false};
    auto job_body = [&body_entered](const std::shared_ptr<UpdateJobSlot>& slot)
    {
        body_entered.store(true);
        // 有界轮询取消标志（防断言失败路径挂死）。
        for (int spin = 0; spin < 2000 && !slot->cancel_requested.load();
             ++spin)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        UpdateJobRegistry::Instance().Finish(slot);
    };

    auto& registry = UpdateJobRegistry::Instance();
    auto  slot =
        registry.TryStart(project.Dir(), /*http=*/nullptr, std::move(job_body));
    ASSERT_NE(slot, nullptr);

    // job 体已进入且 running ⇒ worker 已构造（joinable）。
    bool entered = false;
    for (int spin = 0; spin < 1000 && !entered; ++spin)
    {
        entered = body_entered.load();
        if (!entered)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ASSERT_TRUE(entered);
    EXPECT_TRUE(registry.IsRunning(project.Dir()));
    EXPECT_TRUE(slot->worker.joinable());

    // 置位取消标志 → job 体收尾 Finish → slot idle（防重入解除）。
    slot->cancel_requested.store(true);
    ASSERT_TRUE(registry.WaitForSlotIdleForTest(
        project.Dir(),
        std::chrono::seconds(10)));
    EXPECT_FALSE(registry.IsRunning(project.Dir()));
}

// applyUpdate 失败（异步语义）——受理 ok=true（受理端点不感知后台失败）；
// job 下载失败后终态 entry 携带失败文案（detail 为 DasResult 十进制串），
// pi_dir 旧内容保留 + 检查结果条目保留（用户可重试）。
TEST(MaapiUpdateCommandTest, InvokeCommandApplyUpdateFailureRetainsPending)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->SetNetworkFailure(true);

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());
    session.SetProgressServiceForTest(service.Get());

    PiUpdateInfo info;
    info.has_update = true;
    info.version_name = "v1.2.0";
    info.download_url =
        "https://github.com/owner/repo/releases/download/v1.2.0/"
        "test-project-win-x86_64-v1.2.0.zip";
    info.progress_key = "failure-final-key";
    SeedFoundUpdateOutcome(project.Dir(), info);

    // 受理成功（失败发生在后台 job，受理端点无 error 诊断）。
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"applyUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    auto r1 = ReadJsonInterface(r1_json.Get());
    EXPECT_EQ(CountDiagnosticsBySeverity(r1, "error"), 0u);
    EXPECT_EQ(DocumentRevisionFromApplyResult(r1), 1);
    auto cr = CommandResultOf(r1);
    ASSERT_TRUE(cr.has_value());

    // 等待缝收口 job。
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));

    // 终态 entry：失败文案含 DasResult 十进制（下载失败映射 -10008）。
    ASSERT_FALSE(fake->entries.empty());
    const auto [final_name, final_value] =
        ReadEntryField(fake->entries.back().Get(), 0);
    EXPECT_EQ(final_name, "updateProgress");
    EXPECT_NE(final_value.find("result=-10008"), std::string::npos);

    // pi_dir 旧内容保留 + 检查结果条目保留（可重试）。
    const auto current =
        ReadFileText(project.Dir() / DAS_PATH_LITERAL("interface.json"));
    EXPECT_NE(current.find("1.0.0"), std::string::npos);
    {
        const auto entry = UpdateCheckStore::Instance().Get(project.Dir());
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
    }
}

// commands 投影 applyUpdate.disabled 完全由后端检查结果表条目驱动（无条目
// / 非 FoundUpdate 态 → disabled；FoundUpdate + 有下载链接 → enabled）。
// 前端只按投影 disabled 渲染，业务中立。
TEST(MaapiProjectionTest, CommandsApplyUpdateDisabledDrivenByPendingState)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeStrictProjectionMockCatalog();
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    // 已配置 project_interface 但尚未检查（无检查结果表条目——假路径隔离
    // per-test store 键，ResolvePiDir 只取父目录字符串不触碰文件系统）。
    settings.adapter.interface_path =
        "C:/das-authoring-apply-disabled-no-pending/interface.json";

    // 无条目（未检查）→ applyUpdate disabled。
    auto doc_no_pending = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto apply_no_pending = FindCommand(doc_no_pending, "applyUpdate");
    ASSERT_TRUE(apply_no_pending.has_value());
    EXPECT_TRUE(
        FieldOf(*apply_no_pending, "disabled").as_bool().value_or(false));
    auto check_cmd = FindCommand(doc_no_pending, "checkUpdate");
    ASSERT_TRUE(check_cmd.has_value());
    EXPECT_FALSE(CommandHasDisabled(check_cmd));

    // UpToDate 条目（has_update=false）→ disabled。
    TempUpdateProject up_to_date_project;
    up_to_date_project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));
    {
        UpdateCheckEntry entry;
        entry.state = UpdateCheckEntry::State::UpToDate;
        entry.progress_key = "projection-up-to-date";
        UpdateCheckStore::Instance().PutOutcome(
            up_to_date_project.Dir(),
            std::move(entry));
    }
    auto doc_no_update = ProjectAuthoringDocument(
        MakeUpdateSettings(up_to_date_project.Dir()),
        &catalog,
        {},
        0);
    auto apply_no_update = FindCommand(doc_no_update, "applyUpdate");
    ASSERT_TRUE(apply_no_update.has_value());
    EXPECT_TRUE(
        FieldOf(*apply_no_update, "disabled").as_bool().value_or(false));

    // FoundUpdate 条目且有下载链接 → enabled（缺失 disabled == false）。
    TempUpdateProject found_project;
    found_project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));
    PiUpdateInfo info;
    info.has_update = true;
    info.download_url = "https://example.com/package.zip";
    info.progress_key = "projection-found";
    SeedFoundUpdateOutcome(found_project.Dir(), info);
    auto doc_has_update = ProjectAuthoringDocument(
        MakeUpdateSettings(found_project.Dir()),
        &catalog,
        {},
        0);
    auto apply_has_update = FindCommand(doc_has_update, "applyUpdate");
    ASSERT_TRUE(apply_has_update.has_value());
    EXPECT_FALSE(CommandHasDisabled(apply_has_update));
}

// 更新 section 布局：进度三字段（updateProgress/downloadRate/
// downloadElapsed）置顶作为 section 第一位（更新进行中用户第一眼看到
// 进度状态），其后是命令按钮 row 与版本/日志，其余条目（CDK/代理/通道/
// resourceVersion/maafwVersion）保持 fields[] 声明原序。
// LayoutSectionFieldNames 跳过 row/commandId 叶子且保序。全覆盖契约
// （ValidateFieldLayout）保持。
TEST(MaapiProjectionTest, UpdateSectionProgressLeadsThenVersionAndReleaseNote)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));
    AcceptedSettingsDto settings = MakeUpdateSettings(project.Dir());
    settings.pi.language = "zh_cn";
    PiUpdateInfo info;
    info.has_update = true;
    info.version_name = "v1.2.3";
    info.release_note = "note";
    info.download_url = "https://example.com/package.zip";
    info.progress_key = "layout-found";
    SeedFoundUpdateOutcome(project.Dir(), info);

    auto document = ProjectAuthoringDocument(settings, nullptr, {}, 0);
    auto valid = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;

    auto update_section = FindLayoutSection(document, "update");
    ASSERT_TRUE(update_section.has_value());
    const auto names = LayoutSectionFieldNames(*update_section);
    ASSERT_GE(names.size(), 5u);
    EXPECT_EQ(names[0], "updateProgress");
    EXPECT_EQ(names[1], "downloadRate");
    EXPECT_EQ(names[2], "downloadElapsed");
    EXPECT_EQ(names[3], "updateVersion");
    EXPECT_EQ(names[4], "releaseNote");
    const std::vector<std::string> rest(names.begin() + 5, names.end());
    EXPECT_EQ(
        rest,
        (std::vector<std::string>{
            "adapter.mirrorchyanCdk",
            "adapter.proxyUrl",
            "adapter.updateChannel",
            "resourceVersion",
            "maafwVersion"}))
        << "remaining update section entries must keep fields[] order";
}

// 后台更新 job 运行态投影（后端 registry 状态驱动）：受理响应自身的
// document（ApplyChange dispatch 先于投影，TryStart 已生效）中 checkUpdate
// 与 applyUpdate 均 disabled；slot idle 后投影恢复既有条件——checkUpdate
// 恒可点（无 disabled），applyUpdate 回到 pending 驱动 enabled。
TEST(MaapiProjectionTest, UpdateCommandsDisabledWhileJobRunningAndIdleAfter)
{
    using namespace Das::Plugins::DasMaaPi;

    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));

    const std::string new_interface =
        MakeUpdateInterface("1.2.0", "https://github.com/owner/repo");
    auto http = std::make_shared<FakeUpdateHttpClient>();
    http->AddRoute(
        "/releases/download/v1.2.0/",
        BuildStoredZip({{"interface.json", new_interface}}),
        200);
    http->BlockDownloads();
    BlockedDownloadGuard download_guard(http, project.Dir());

    MaapiAuthoringSession session(MakeUpdateSettings(project.Dir()));
    session.SetHttpClientForTest(http.get());
    PiUpdateInfo info;
    info.has_update = true;
    info.version_name = "v1.2.0";
    info.download_url =
        "https://github.com/owner/repo/releases/download/v1.2.0/"
        "test-project-win-x86_64-v1.2.0.zip";
    SeedFoundUpdateOutcome(project.Dir(), info);

    // 受理 → 响应 document 已含双键 disabled。
    DasPtr<Das::ExportInterface::IDasJson> r1_json;
    auto                                   c1 = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"invokeCommand\","
        "\"payload\":{\"commandId\":\"applyUpdate\"}}");
    ASSERT_EQ(session.ApplyChange(c1.Get(), r1_json.Put()), DAS_S_OK);
    auto r1 = ReadJsonInterface(r1_json.Get());
    ASSERT_EQ(CountDiagnosticsBySeverity(r1, "error"), 0u);
    auto document = DocumentFromApplyResult(r1);
    auto check_cmd = FindCommand(document, "checkUpdate");
    ASSERT_TRUE(check_cmd.has_value());
    EXPECT_TRUE(FieldOf(*check_cmd, "disabled").as_bool().value_or(false));
    auto apply_cmd = FindCommand(document, "applyUpdate");
    ASSERT_TRUE(apply_cmd.has_value());
    EXPECT_TRUE(FieldOf(*apply_cmd, "disabled").as_bool().value_or(false));

    // 释放 + 等待 idle → 投影恢复：checkUpdate 无 disabled，applyUpdate
    // 回到检查结果条目条件（FoundUpdate + 有下载链接 → enabled，缺失
    // disabled == false）。
    http->ReleaseDownload();
    ASSERT_TRUE(
        UpdateJobRegistry::Instance().WaitForSlotIdleForTest(
            project.Dir(),
            std::chrono::seconds(10)));
    auto idle_document = ProjectAuthoringDocument(
        MakeUpdateSettings(project.Dir()),
        nullptr,
        {},
        0);
    auto idle_check = FindCommand(idle_document, "checkUpdate");
    ASSERT_TRUE(idle_check.has_value());
    EXPECT_FALSE(CommandHasDisabled(idle_check));
    auto idle_apply = FindCommand(idle_document, "applyUpdate");
    ASSERT_TRUE(idle_apply.has_value());
    EXPECT_FALSE(CommandHasDisabled(idle_apply));
}

// D-89-07 修订：项目设置 section 投影两个只读版本字段。resourceVersion 值 =
// catalog.version；maafwVersion 值 = MaaVersion 动态符号（测试构建未映射 →
// 空串）。两字段 readonly:true、valueType 4 / inputType 0；fieldLayout 项目设置
// section entries 覆盖两者；ValidateAuthoringDocument（含 ValidateFieldLayout
// 全覆盖）不 reject。
TEST(MaapiProjectionTest, ProjectAuthoringDocumentProjectsReadonlyVersionFields)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    catalog.version = "1.2.3";
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    // 已配置 project_interface 的 entry（版本字段按配置存在性投影）。
    settings.adapter.interface_path =
        "C:/das-authoring-version-fields/interface.json";

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto valid = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;

    auto resource = FindRootField(document, "resourceVersion");
    ASSERT_TRUE(resource.has_value());
    EXPECT_EQ(FieldOf(*resource, "value").as_string().value_or(""), "1.2.3");
    EXPECT_TRUE(FieldOf(*resource, "readonly").as_bool().value_or(false));
    EXPECT_EQ(FieldOf(*resource, "valueType").as_sint().value_or(-1), 4);
    EXPECT_EQ(FieldOf(*resource, "inputType").as_sint().value_or(-1), 0);

    auto maafw = FindRootField(document, "maafwVersion");
    ASSERT_TRUE(maafw.has_value());
    EXPECT_TRUE(FieldOf(*maafw, "readonly").as_bool().value_or(false));
    EXPECT_EQ(FieldOf(*maafw, "valueType").as_sint().value_or(-1), 4);
    EXPECT_EQ(FieldOf(*maafw, "inputType").as_sint().value_or(-1), 0);
    // 测试构建（DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY）MaaVersion 未映射 → 空串
    // （null-safe，T-89-03-05 C 边界拷贝）。
    EXPECT_EQ(FieldOf(*maafw, "value").as_string().value_or("x"), "");

    // fieldLayout 更新 section entries 覆盖两个版本字段（D-89-09 增量：
    // 版本字段从项目设置 section 独立到更新 section）。
    auto update = FindLayoutSection(document, "update");
    ASSERT_TRUE(update.has_value());
    auto names = LayoutSectionFieldNames(*update);
    EXPECT_NE(
        std::find(names.begin(), names.end(), "resourceVersion"),
        names.end());
    EXPECT_NE(
        std::find(names.begin(), names.end(), "maafwVersion"),
        names.end());
    EXPECT_NE(
        std::find(names.begin(), names.end(), "adapter.mirrorchyanCdk"),
        names.end());
    EXPECT_NE(
        std::find(names.begin(), names.end(), "adapter.updateChannel"),
        names.end());
}

// catalog 缺席（已配置 interfacePath 但解析失败——生产上 catalog=nullptr
// 的既有文档形态）时版本字段仍投影，值回退空串，fieldLayout 更新 section
// （update 桶）仍覆盖。已配置行为不变（条件挂配置存在性，非全局砍分区）；
// 全新未配置 entry 的干净表单由
// RootFieldLayoutCoversAdapterFieldsWhenCatalogAbsent 守卫。
TEST(MaapiProjectionTest, ProjectAuthoringDocumentVersionFieldsCatalogAbsent)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    settings.adapter.interface_path =
        "C:/das-authoring-version-fields-catalog-absent/interface.json";

    auto document = ProjectAuthoringDocument(settings, nullptr, {}, 0);
    auto valid = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;

    auto resource = FindRootField(document, "resourceVersion");
    ASSERT_TRUE(resource.has_value());
    EXPECT_EQ(FieldOf(*resource, "value").as_string().value_or(""), "");
    EXPECT_TRUE(FieldOf(*resource, "readonly").as_bool().value_or(false));

    auto maafw = FindRootField(document, "maafwVersion");
    ASSERT_TRUE(maafw.has_value());
    EXPECT_TRUE(FieldOf(*maafw, "readonly").as_bool().value_or(false));

    auto update = FindLayoutSection(document, "update");
    ASSERT_TRUE(update.has_value());
    auto names = LayoutSectionFieldNames(*update);
    EXPECT_NE(
        std::find(names.begin(), names.end(), "resourceVersion"),
        names.end());
    EXPECT_NE(
        std::find(names.begin(), names.end(), "maafwVersion"),
        names.end());
}

// ============================================================================
// welcome 字段投影（ProjectInterface V2 协议 welcome 顶层字段）
// ============================================================================
//
// welcome 是 PI interface.json 顶层静态 catalog 字段（与 license/description
// 并列），由 MaaPI 投影层在 ProjectAuthoringDocument 时挂入 document.fields[]
// + fieldLayout[]。welcome 是初始 document 内容（非 apply change），Core
// schema/validator 零改动——welcome field 复用既有 inputType=7 markdown 渲染
// 链路（与 releaseNote 同构三元组 valueType=4/inputType=7/readonly=true）。
//
// 业务中立：welcome 是通用 authoring doc 元素，Core 不识别 Provider 业务前缀，
// 前端按 fieldLayout 数据驱动渲染（inputType=7 路由既有，零前端改动）。

// catalog 含 welcome 时 document.fields[] 含 welcome field，三元组
// (valueType=4 / inputType=7 / readonly=true) 与 releaseNote 同构。
// 内容非 $ 前缀时按直接 markdown 文本投影。
TEST(MaapiProjectionTest, WelcomeFieldMarkdownReadonly)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    catalog.welcome = "## Welcome to DAS";
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto valid = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;

    auto field = FindRootField(document, "welcome");
    ASSERT_TRUE(field.has_value());
    auto field_obj = field->as_object();
    ASSERT_TRUE(field_obj.has_value());
    EXPECT_EQ(
        (*field_obj)[std::string_view("valueType")].as_sint().value_or(-1),
        4);
    EXPECT_EQ(
        (*field_obj)[std::string_view("inputType")].as_sint().value_or(-1),
        7);
    EXPECT_TRUE(
        (*field_obj)[std::string_view("readonly")].as_bool().value_or(false));
    const std::string value = std::string(
        (*field_obj)[std::string_view("value")].as_string().value_or(""));
    EXPECT_NE(value.find("Welcome to DAS"), std::string::npos);
}

// 业务中立空值守卫：catalog 无 welcome（std::nullopt）时不投影空 welcome
// field（避免前端渲染空面板）。
TEST(MaapiProjectionTest, WelcomeFieldAbsentWhenCatalogLacksWelcome)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    // welcome 保持 std::nullopt（默认）
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto valid = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;

    EXPECT_FALSE(FindRootField(document, "welcome").has_value());
}

// welcome field 应独立成 section（id="welcome"），不落入 BuildRootFieldLayout
// 的兜底桶（project_entries）。若 welcome 未显式登记分类桶，会混入「项目设置」
// section，前端按数据驱动渲染会显示在错误面板里。
TEST(MaapiProjectionTest, WelcomeSectionProjectedWhenPresent)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    catalog.welcome = "## Hello";
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto valid = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;

    auto section = FindLayoutSection(document, "welcome");
    ASSERT_TRUE(section.has_value());
    auto names = LayoutSectionFieldNames(*section);
    EXPECT_NE(std::ranges::find(names, "welcome"), names.end());

    // welcome 不应同时出现在 project section（防兜底桶双计入）。
    auto project = FindLayoutSection(document, "project");
    ASSERT_TRUE(project.has_value());
    auto project_names = LayoutSectionFieldNames(*project);
    EXPECT_EQ(std::ranges::find(project_names, "welcome"), project_names.end());
}

// welcome section 顺序位于 update section 之后、project section 之前。
// 检查结果表有条目 + welcome → fieldLayout 顺序含 [update, welcome,
// project]。
TEST(MaapiProjectionTest, WelcomeSectionOrderingBeforeProject)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    catalog.welcome = "## Hello";
    TempUpdateProject project;
    project.WriteInterface(
        MakeUpdateInterface("1.0.0", "https://github.com/owner/repo"));
    AcceptedSettingsDto settings = MakeUpdateSettings(project.Dir());
    settings.pi.language = "zh_cn";
    PiUpdateInfo info;
    info.has_update = true;
    info.version_name = "v1.2.3";
    info.release_note = "note";
    info.download_url = "https://example.com/package.zip";
    info.progress_key = "welcome-order-found";
    SeedFoundUpdateOutcome(project.Dir(), info);

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    // 枚举 fieldLayout[] section.id 序列。
    std::vector<std::string> section_ids;
    {
        auto doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        auto layout = (*doc_obj)[std::string_view("fieldLayout")].as_array();
        ASSERT_TRUE(layout.has_value());
        for (auto it = layout->begin(); it != layout->end(); ++it)
        {
            auto section = it->as_object();
            if (!section)
            {
                continue;
            }
            section_ids.emplace_back(
                (*section)[std::string_view("id")].as_string().value_or(""));
        }
    }

    auto update_it = std::ranges::find(section_ids, "update");
    auto welcome_it = std::ranges::find(section_ids, "welcome");
    auto project_it = std::ranges::find(section_ids, "project");

    ASSERT_NE(update_it, section_ids.end()) << "update section missing";
    ASSERT_NE(welcome_it, section_ids.end()) << "welcome section missing";
    ASSERT_NE(project_it, section_ids.end()) << "project section missing";

    EXPECT_LT(update_it, welcome_it)
        << "update section must come before welcome";
    EXPECT_LT(welcome_it, project_it)
        << "welcome section must come before project";
}

// 无 pending_update 时 welcome section 应在 fieldLayout 首位。
TEST(MaapiProjectionTest, WelcomeTopNoUpdate)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    catalog.welcome = "## Hello";
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    auto layout = (*doc_obj)[std::string_view("fieldLayout")].as_array();
    ASSERT_TRUE(layout.has_value());
    ASSERT_FALSE(layout->empty());
    auto first = layout->begin()->as_object();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(
        std::string((*first)[std::string_view("id")].as_string().value_or("")),
        "welcome");
}

// catalog 无 welcome 时不应 emit 空 welcome section（避免前端渲染空
// Expander）。
TEST(MaapiProjectionTest, WelcomeSectionAbsentWhenNoWelcome)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    // welcome 保持 std::nullopt
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto valid = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;

    EXPECT_FALSE(FindLayoutSection(document, "welcome").has_value());
}

// welcome 内容支持 PI i18n：值以 '$' 开头时按 translations[lang][key] 解析为
// 翻译文本。经 ResolveI18nOrDiag 走与 releaseNote / section label 一致的 i18n
// 链路（无新机制）。
TEST(MaapiProjectionTest, WelcomeI18nKeyResolved)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    catalog.welcome = "$greeting.welcome";
    catalog.translations["zh_cn"]["greeting.welcome"] = "# 欢迎";
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    auto field = FindRootField(document, "welcome");
    ASSERT_TRUE(field.has_value());
    auto field_obj = field->as_object();
    ASSERT_TRUE(field_obj.has_value());
    const std::string value = std::string(
        (*field_obj)[std::string_view("value")].as_string().value_or(""));
    EXPECT_EQ(value, "# 欢迎");
}

// welcome \$key 缺译时不应投影该 field（避免用户看到 \$key 字面量）。同时
// projection_diagnostics 应含 warning diagnostic（ResolveI18nOrDiag 缺译时
// 上报）。
TEST(MaapiProjectionTest, WelcomeI18nMissingKeyNotProjected)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    catalog.welcome = "$nonexistent.key";
    // translations 不含 nonexistent.key
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";

    std::vector<PiDiagnosticDto> diags;
    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0, &diags);

    // \$key 缺译 → 不投影 welcome field（避免显示 \$key 字面量）。
    EXPECT_FALSE(FindRootField(document, "welcome").has_value());

    // ResolveI18nOrDiag 应已 emit warning diagnostic。
    bool found_warning = false;
    for (const auto& d : diags)
    {
        if (d.severity == "warning"
            && d.message.find("nonexistent.key") != std::string::npos)
        {
            found_warning = true;
            break;
        }
    }
    EXPECT_TRUE(found_warning);
}

// ===== controllerAdbDevice 字段 + adbDevice 字段投影 =====

// controllerAdbDevice 持久化字段对称 parse/serialize round-trip。值是设备
// address（用户 dropdown 选中项的稳定 key），完整六字段不进 AcceptedSettings
// （运行时从 device_cache_ 取 Toolkit 推荐值不持久化）。
TEST(MaapiProjectionTest, ControllerAdbDeviceRoundTrip)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto src;
    src.pi.controller_adb_device = "127.0.0.1:5555";

    auto serialized = SerializeAcceptedSettings(src);
    auto parsed = ParseAcceptedSettings(serialized);

    ASSERT_TRUE(parsed.pi.controller_adb_device.has_value());
    EXPECT_EQ(*parsed.pi.controller_adb_device, "127.0.0.1:5555");

    // 缺失字段 round-trip 回 nullopt（不默认构造空串）。
    AcceptedSettingsDto empty;
    auto                empty_ser = SerializeAcceptedSettings(empty);
    auto                empty_parsed = ParseAcceptedSettings(empty_ser);
    EXPECT_FALSE(empty_parsed.pi.controller_adb_device.has_value());
}

// adbDevice 字段仅当 controller 类型为 Adb 时投影。win32 / gamepad / dbg 等
// controller 不显示 ADB 设备 dropdown（IsCurrentControllerAdb 守卫）。
TEST(MaapiProjectionTest, AdbDeviceFieldProjectedOnlyForAdbController)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeStrictProjectionMockCatalog();
    ASSERT_EQ(catalog.controllers.size(), 1u);
    ASSERT_EQ(catalog.controllers.front().dto.type, "Adb");

    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    settings.pi.controller_name = catalog.controllers.front().dto.name;

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto valid = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;

    EXPECT_TRUE(FindRootField(document, "adbDevice").has_value());

    // 切换 controller 类型为 Win32 → adbDevice 字段消失。
    auto& controller = catalog.controllers.front();
    controller.dto.type = "Win32";
    auto document_win32 = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    EXPECT_FALSE(FindRootField(document_win32, "adbDevice").has_value());
}

// adbDevice.candidates 从 device_cache 构造：value=address, label=name
// （name 空 fallback address）。cache 非空 → 候补数组长度匹配。
TEST(MaapiProjectionTest, AdbDeviceCandidatesFromCache)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeStrictProjectionMockCatalog();
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    settings.pi.controller_name = catalog.controllers.front().dto.name;

    std::vector<AdbDeviceInfo> cache;
    AdbDeviceInfo              dev_emu;
    dev_emu.name = "emulator-5554";
    dev_emu.adb_path = "/usr/bin/adb";
    dev_emu.address = "127.0.0.1:5554";
    dev_emu.screencap_methods = 0x1;
    dev_emu.input_methods = 0x2;
    dev_emu.config = "{}";
    cache.push_back(dev_emu);
    AdbDeviceInfo dev_real;
    dev_real.name = "Pixel-7";
    dev_real.address = "192.168.1.42:5555";
    cache.push_back(dev_real);

    auto document =
        ProjectAuthoringDocument(settings, &catalog, {}, 0, nullptr, &cache);

    auto field = FindRootField(document, "adbDevice");
    ASSERT_TRUE(field.has_value());
    auto field_obj = field->as_object();
    ASSERT_TRUE(field_obj.has_value());
    ASSERT_TRUE(field_obj->contains(std::string_view("candidates")));
    auto candidates = (*field_obj)[std::string_view("candidates")].as_array();
    ASSERT_TRUE(candidates.has_value());
    EXPECT_EQ(candidates->size(), 2u);
    // 第一项：value=address, label=name
    auto first = (*candidates)[0].as_object();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(
        std::string(
            (*first)[std::string_view("value")].as_string().value_or("")),
        "127.0.0.1:5554");
    EXPECT_EQ(
        std::string(
            (*first)[std::string_view("label")].as_string().value_or("")),
        "emulator-5554");
}

// cache=nullptr 时 candidates 空数组，字段仍投影（前端 dropdown 空候补，
// 触发用户点 discover 按钮扫描）。
TEST(MaapiProjectionTest, AdbDeviceCandidatesEmptyWhenCacheNull)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeStrictProjectionMockCatalog();
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    settings.pi.controller_name = catalog.controllers.front().dto.name;

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto valid = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;

    auto field = FindRootField(document, "adbDevice");
    ASSERT_TRUE(field.has_value());
    auto field_obj = field->as_object();
    ASSERT_TRUE(field_obj.has_value());
    ASSERT_TRUE(field_obj->contains(std::string_view("candidates")));
    auto candidates = (*field_obj)[std::string_view("candidates")].as_array();
    ASSERT_TRUE(candidates.has_value());
    EXPECT_EQ(candidates->size(), 0u);
}

// adbDevice 与 controllerName 横排成 row（fieldLayout 内）；其余
// project_entries 字段（resourceName / presetName / language / adapter.*
// 等）仍各自单独 leaf。
TEST(
    MaapiProjectionTest,
    AdbDeviceAndControllerNameEmittedAsRowInProjectSection)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeStrictProjectionMockCatalog();
    AcceptedSettingsDto settings;
    settings.pi.language = "zh_cn";
    settings.pi.controller_name = catalog.controllers.front().dto.name;

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto valid = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(valid.valid)
        << "error_kind=" << valid.error_kind << " message=" << valid.message;

    auto layout_opt = FindLayoutSection(document, "project");
    ASSERT_TRUE(layout_opt.has_value());
    auto entries = (*layout_opt)
                       .as_object()
                       .value()[std::string_view("entries")]
                       .as_array();
    ASSERT_TRUE(entries.has_value());
    // 找到含 row 的 entry，row 应含三叶子：controllerName / adbDevice
    // （fieldName 叶子）+ discoverAdbDevices （commandId 叶子）。
    bool found_row = false;
    for (auto&& entry : *entries)
    {
        auto entry_obj = entry.as_object();
        if (!entry_obj || !entry_obj->contains(std::string_view("row")))
        {
            continue;
        }
        auto row = (*entry_obj)[std::string_view("row")].as_array();
        ASSERT_TRUE(row.has_value());
        EXPECT_EQ(row->size(), 3u);
        auto leaf0 = (*row)[0].as_object();
        auto leaf1 = (*row)[1].as_object();
        auto leaf2 = (*row)[2].as_object();
        ASSERT_TRUE(
            leaf0.has_value() && leaf1.has_value() && leaf2.has_value());
        EXPECT_EQ(
            std::string(
                (*leaf0)[std::string_view("fieldName")].as_string().value_or(
                    "")),
            "controllerName");
        EXPECT_EQ(
            std::string(
                (*leaf1)[std::string_view("fieldName")].as_string().value_or(
                    "")),
            "adbDevice");
        EXPECT_EQ(
            std::string(
                (*leaf2)[std::string_view("commandId")].as_string().value_or(
                    "")),
            "discoverAdbDevices");
        found_row = true;
        break;
    }
    EXPECT_TRUE(found_row);

    // commands[] 应含 discoverAdbDevices（IsCurrentControllerAdb 守卫通过）。
    auto commands_opt =
        document.as_object().value()[std::string_view("commands")].as_array();
    ASSERT_TRUE(commands_opt.has_value());
    bool found_cmd = false;
    for (auto&& cmd : *commands_opt)
    {
        auto cmd_obj = cmd.as_object();
        if (!cmd_obj)
        {
            continue;
        }
        if ((*cmd_obj)[std::string_view("id")].as_string().value_or("")
            == "discoverAdbDevices")
        {
            found_cmd = true;
            break;
        }
    }
    EXPECT_TRUE(found_cmd);
}

// ===== discoverAdbDevices command handler 闭环测试（session 级）=====

namespace
{
    // 经 ApplyInvokeCommandChange 的 adb_enumerator 参数注入的 fake
    // 扫描结果，供 discoverAdbDevices 命令链路单测复用（镜像
    // AdbDeviceFinderTest 范式）。
    std::vector<Das::Plugins::DasMaaPi::AdbDeviceInfo> MakeTwoFakeDevices()
    {
        using namespace Das::Plugins::DasMaaPi;
        std::vector<AdbDeviceInfo> devices;
        AdbDeviceInfo              emu;
        emu.name = "emulator-5554";
        emu.adb_path = "/fake/adb";
        emu.address = "127.0.0.1:5554";
        emu.screencap_methods = 0x1;
        emu.input_methods = 0x2;
        emu.config = "{}";
        devices.push_back(emu);
        AdbDeviceInfo real;
        real.name = "Pixel-7";
        real.address = "192.168.1.42:5555";
        real.adb_path = "/fake/adb";
        real.screencap_methods = 0x4;
        real.input_methods = 0x8;
        real.config = "{}";
        devices.push_back(real);
        return devices;
    }
} // namespace

// discoverAdbDevices 命令经 ApplyInvokeCommandChange（adb_enumerator 显式
// 依赖注入）→ AdbDeviceFinder 扫描 → 填 session.device_cache_（size==2）→
// 返回 {ok=true, command_result nullopt}（设备进 document field candidates，
// 不走 commandResult.data — 前端 dropdown 现成机制渲染 candidates）。
TEST(
    MaapiAdbDeviceCommandTest,
    DiscoverAdbDevicesFillsCacheAndOmitsCommandResult)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto   settings;
    MaapiAuthoringSession session(settings);

    // 直接调 ApplyInvokeCommandChange 注入 fake 扫描器（显式参数，非全局
    // seam）。
    std::vector<PiDiagnosticDto> diag;
    auto                         payload = Das::Utils::ParseYyjsonFromString(
                                               R"({"commandId":"discoverAdbDevices"})")
                                               .value();
    auto                         result = ApplyInvokeCommandChange(
        session,
        settings,
        payload,
        diag,
        nullptr,
        nullptr,
        []() { return MakeTwoFakeDevices(); });

    EXPECT_TRUE(result.ok);
    size_t error_count = 0;
    for (const auto& d : diag)
    {
        if (d.severity == "error")
        {
            ++error_count;
        }
    }
    EXPECT_EQ(error_count, 0u);
    // device_cache_ 暂态写入 2 个设备。
    EXPECT_EQ(session.DeviceCache().size(), 2u);
    EXPECT_EQ(session.DeviceCache().front().address, "127.0.0.1:5554");
    // commandResult 不存在（设备进 document field candidates）。
    EXPECT_FALSE(result.command_result.has_value());
}

// discoverAdbDevices 后 GetDocument 重算 → adbDevice.candidates 反映 cache
// 内容（2 个 device 经 document field 流到前端 dropdown）。ApplyInvokeCommand
// Change 注入 fake 填 cache，GetDocument 投影验证 candidates 链路。
TEST(MaapiAdbDeviceCommandTest, DiscoverAdbDevicesAppliesToDocumentCandidates)
{
    using namespace Das::Plugins::DasMaaPi;

    // 构造 AcceptedSettingsDto 指向 interface_controller.jsonc（含 Adb
    // controller 名 "Android"，确保 IsCurrentControllerAdb 守卫通过）。
    auto settings_json = R"({"adapter":{"interfacePath":")"
                         + Das::Utils::PathToGenericUtf8(
                             FixturePath("interface_controller.jsonc"))
                         + R"("}})";
    auto settings = ParseAcceptedSettings(
        Das::Utils::ParseYyjsonFromString(settings_json).value());
    MaapiAuthoringSession session(settings);

    std::vector<PiDiagnosticDto> diag;
    auto                         payload = Das::Utils::ParseYyjsonFromString(
                                               R"({"commandId":"discoverAdbDevices"})")
                                               .value();
    EXPECT_TRUE(ApplyInvokeCommandChange(
                    session,
                    settings,
                    payload,
                    diag,
                    nullptr,
                    nullptr,
                    []() { return MakeTwoFakeDevices(); })
                    .ok);

    // GetDocument 投影 device_cache_ → adbDevice.candidates。
    DasPtr<Das::ExportInterface::IDasJson> doc_json;
    ASSERT_EQ(session.GetDocument(nullptr, doc_json.Put()), DAS_S_OK);
    auto document = DocumentFromApplyResult(ReadJsonInterface(doc_json.Get()));
    auto field = FindRootField(document, "adbDevice");
    ASSERT_TRUE(field.has_value());
    auto field_obj = field->as_object();
    ASSERT_TRUE(field_obj.has_value());
    ASSERT_TRUE(field_obj->contains(std::string_view("candidates")));
    auto candidates = (*field_obj)[std::string_view("candidates")].as_array();
    ASSERT_TRUE(candidates.has_value());
    EXPECT_EQ(candidates->size(), 2u);
}

// enumerator 返回空 → device_cache_ 空。test build 下 MaaToolkit 符号未解析
// （adb_* null），故走 toolkit-unavailable 分支 emit warning（仍 ok=true
// 降级不崩）。验证 zero-device 路径不崩，前端 dropdown 空候补。
TEST(
    MaapiAdbDeviceCommandTest,
    DiscoverAdbDevicesEmptyEnumeratorYieldsEmptyCache)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto   settings;
    MaapiAuthoringSession session(settings);

    std::vector<PiDiagnosticDto> diag;
    auto                         payload = Das::Utils::ParseYyjsonFromString(
                                               R"({"commandId":"discoverAdbDevices"})")
                                               .value();
    auto                         result = ApplyInvokeCommandChange(
        session,
        settings,
        payload,
        diag,
        nullptr,
        nullptr,
        []() -> std::vector<AdbDeviceInfo> { return {}; });

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(session.DeviceCache().size(), 0u);
    // test build toolkit 不可用 → emit warning（非 info）。
    size_t warning_count = 0;
    for (const auto& d : diag)
    {
        if (d.severity == "warning")
        {
            ++warning_count;
        }
    }
    EXPECT_GE(warning_count, 1u);
}

// ===== CompileMaapi 设备 cache → controller_init_params 链路测试 =====

// settings.pi.controller_adb_device + device_cache 含该 address → 编译输出
// 的 controller_init_params.adb 六字段取自 cache（覆盖 raw_json）。
TEST(MaapiCompileDeviceCacheTest, SelectedDeviceInCacheOverridesRawJson)
{
    using namespace Das::Plugins::DasMaaPi;

    // interface_controller.jsonc fixture 含一个 Adb controller 名为 "Android"
    // address="127.0.0.1:5555"（raw_json 默认值），cache 提供同 address 的设备
    // 但 adb_path/screencap/input/config 不同 → 编译期应取 cache 值。
    auto parsed = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(parsed.ok);

    AcceptedSettingsDto settings;
    settings.adapter.interface_path =
        Das::Utils::PathToUtf8(FixturePath("interface_controller.jsonc"));
    settings.pi.controller_name = "Android";
    settings.pi.controller_adb_device = "127.0.0.1:5554";

    std::vector<AdbDeviceInfo> cache;
    AdbDeviceInfo              dev;
    dev.name = "emulator-5554";
    dev.adb_path = "/cache/adb";
    dev.address = "127.0.0.1:5554";
    dev.screencap_methods = 0x1;
    dev.input_methods = 0x2;
    dev.config = R"({"cache":true})";
    cache.push_back(dev);

    auto compile = CompileMaapi(settings, parsed.catalog, "preview", &cache);
    ASSERT_TRUE(compile.ok);
    ASSERT_TRUE(compile.execution_input.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params->adb.has_value());
    const auto& adb =
        *compile.execution_input->maapi.controller_init_params->adb;
    EXPECT_EQ(adb.address, "127.0.0.1:5554");
    EXPECT_EQ(adb.adb_path, "/cache/adb");
    EXPECT_EQ(adb.screencap_method, 0x1u);
    EXPECT_EQ(adb.input_method, 0x2u);
    EXPECT_EQ(adb.config_json, R"({"cache":true})");
}

// settings.pi.controller_adb_device 指向不在 cache 的 address → fallback 用
// raw_json 字段 + warning diagnostic。
TEST(MaapiCompileDeviceCacheTest, SelectedDeviceNotInCacheFallsBackWithWarning)
{
    using namespace Das::Plugins::DasMaaPi;

    auto parsed = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(parsed.ok);

    AcceptedSettingsDto settings;
    settings.adapter.interface_path =
        Das::Utils::PathToUtf8(FixturePath("interface_controller.jsonc"));
    settings.pi.controller_name = "Android";
    // 指向不在 cache 的 address。
    settings.pi.controller_adb_device = "ghost:9999";

    std::vector<AdbDeviceInfo> cache;
    AdbDeviceInfo              dev;
    dev.address = "127.0.0.1:5554";
    cache.push_back(dev);

    auto compile = CompileMaapi(settings, parsed.catalog, "preview", &cache);
    ASSERT_TRUE(compile.ok);
    ASSERT_TRUE(compile.execution_input.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params->adb.has_value());
    const auto& adb =
        *compile.execution_input->maapi.controller_init_params->adb;
    // 用 raw_json 既有字段（fixture 中 address="127.0.0.1:5555"，adbPath 缺省
    // → fallback "adb"）。
    EXPECT_EQ(adb.address, "127.0.0.1:5555");
    EXPECT_EQ(adb.adb_path, "adb");
    // warning diagnostic 记录 cache miss。
    bool found_warning = false;
    for (const auto& diag : compile.diagnostics)
    {
        if (diag.severity == "warning"
            && diag.message.find("ghost:9999") != std::string::npos)
        {
            found_warning = true;
            break;
        }
    }
    EXPECT_TRUE(found_warning);
}

// settings.pi.controller_adb_device=nullopt + cache 非空 → 完全用 raw_json
// （cache 被忽略，既有行为保持）。验证无选中设备时 cache 不强制覆盖。
TEST(MaapiCompileDeviceCacheTest, EmptySelectedDeviceUsesRawJsonAndIgnoresCache)
{
    using namespace Das::Plugins::DasMaaPi;

    auto parsed = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(parsed.ok);

    AcceptedSettingsDto settings;
    settings.adapter.interface_path =
        Das::Utils::PathToUtf8(FixturePath("interface_controller.jsonc"));
    settings.pi.controller_name = "Android";
    // controller_adb_device 留空（nullopt）。

    std::vector<AdbDeviceInfo> cache;
    AdbDeviceInfo              dev;
    dev.address = "127.0.0.1:5554";
    cache.push_back(dev);

    auto compile = CompileMaapi(settings, parsed.catalog, "preview", &cache);
    ASSERT_TRUE(compile.ok);
    ASSERT_TRUE(compile.execution_input.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params->adb.has_value());
    const auto& adb =
        *compile.execution_input->maapi.controller_init_params->adb;
    EXPECT_EQ(adb.address, "127.0.0.1:5555");
    EXPECT_EQ(adb.adb_path, "adb");
    // 无 cache miss warning（用户未选设备，不需要查 cache）。
    bool found_cache_warning = false;
    for (const auto& diag : compile.diagnostics)
    {
        if (diag.severity == "warning" && diag.code == "adb-device-cache-miss")
        {
            found_cache_warning = true;
            break;
        }
    }
    EXPECT_FALSE(found_cache_warning);
}

// ===== CompileMaapi 设备现扫四形态 + 地址未选择（execution 装配段语义） =====
// 设备六字段装配只发生在 purpose=execution 编译——按地址唯一命中选中
// （地址是持久化选择键，唯一才选中；不做名称消歧），扫不到/多台同地址
// =遇缺即错具名诊断；preview 编译零设备动作（不扫、不查缓存、不填六字
// 段）；地址未选择不因设备失败（运行期 adb-device-missing 兜底）。
// 设备表经第 4 参注入（枚举器形态前身为 device_cache 指针，语义同为
// 「本次可见设备清单」）。

namespace
{
    Das::Plugins::DasMaaPi::AcceptedSettingsDto MakeAdbScanFixtureSettings(
        const char* selected_address)
    {
        Das::Plugins::DasMaaPi::AcceptedSettingsDto settings;
        settings.adapter.interface_path =
            Das::Utils::PathToUtf8(FixturePath("interface_controller.jsonc"));
        settings.pi.controller_name = "Android";
        if (selected_address != nullptr)
        {
            settings.pi.controller_adb_device = selected_address;
        }
        return settings;
    }

    Das::Plugins::DasMaaPi::AdbDeviceInfo MakeScanDevice(const char* address)
    {
        Das::Plugins::DasMaaPi::AdbDeviceInfo device;
        device.name = std::string("emulator@") + address;
        device.adb_path = "/scan/adb";
        device.address = address;
        device.screencap_methods = 0x11;
        device.input_methods = 0x22;
        device.config = R"({"scan":true})";
        return device;
    }

    const Das::Plugins::DasMaaPi::PiDiagnosticDto* FindDiagnosticByCode(
        const Das::Plugins::DasMaaPi::CompileResultDto& compile,
        const char*                                     code)
    {
        for (const auto& diag : compile.diagnostics)
        {
            if (diag.code == code)
            {
                return &diag;
            }
        }
        return nullptr;
    }
} // namespace

// execution + 已选地址 + 扫描零设备 → 遇缺即错：ok=false + 具名
// adb-device-missing 诊断（int_code 复用 -10010 语义，不新配号）。
TEST(MaapiCompileAdbDeviceScanTest, ExecutionMissingDeviceFailsCompile)
{
    using namespace Das::Plugins::DasMaaPi;

    auto parsed = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(parsed.ok);

    const auto settings = MakeAdbScanFixtureSettings("127.0.0.1:5554");
    // 扫描结果为空表（设备已拔出 / toolkit 不可用降级同为空表形态）。
    const std::vector<AdbDeviceInfo> devices;

    auto compile =
        CompileMaapi(settings, parsed.catalog, "execution", &devices);
    EXPECT_FALSE(compile.ok);
    const auto* diag = FindDiagnosticByCode(compile, "adb-device-missing");
    ASSERT_NE(diag, nullptr);
    EXPECT_EQ(diag->severity, "error");
    ASSERT_TRUE(diag->int_code.has_value());
    EXPECT_EQ(*diag->int_code, DAS_E_MAAPI_ADB_DEVICE_MISSING);
}

// execution + N 台设备但无地址命中 → 同为设备未找到（遇缺即错）。
TEST(MaapiCompileAdbDeviceScanTest, ExecutionNoAddressMatchFailsCompile)
{
    using namespace Das::Plugins::DasMaaPi;

    auto parsed = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(parsed.ok);

    const auto settings = MakeAdbScanFixtureSettings("127.0.0.1:5554");
    std::vector<AdbDeviceInfo> devices;
    devices.push_back(MakeScanDevice("127.0.0.1:5556"));
    devices.push_back(MakeScanDevice("127.0.0.1:5558"));

    auto compile =
        CompileMaapi(settings, parsed.catalog, "execution", &devices);
    EXPECT_FALSE(compile.ok);
    const auto* diag = FindDiagnosticByCode(compile, "adb-device-missing");
    ASSERT_NE(diag, nullptr);
    EXPECT_EQ(diag->severity, "error");
}

// execution + 多台设备同地址 → 唯一才选中：ok=false + 具名
// adb-device-ambiguous 诊断。
TEST(MaapiCompileAdbDeviceScanTest, ExecutionAmbiguousAddressFailsCompile)
{
    using namespace Das::Plugins::DasMaaPi;

    auto parsed = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(parsed.ok);

    const auto settings = MakeAdbScanFixtureSettings("127.0.0.1:5554");
    std::vector<AdbDeviceInfo> devices;
    devices.push_back(MakeScanDevice("127.0.0.1:5554"));
    devices.push_back(MakeScanDevice("127.0.0.1:5554"));

    auto compile =
        CompileMaapi(settings, parsed.catalog, "execution", &devices);
    EXPECT_FALSE(compile.ok);
    const auto* diag = FindDiagnosticByCode(compile, "adb-device-ambiguous");
    ASSERT_NE(diag, nullptr);
    EXPECT_EQ(diag->severity, "error");
}

// execution + 单设备地址命中 → 六字段 typed init params 全部来自活扫描值
// （地址/adbPath/screencap/input/config 覆盖 raw_json）。
TEST(MaapiCompileAdbDeviceScanTest, ExecutionScanHitFillsTypedInitParams)
{
    using namespace Das::Plugins::DasMaaPi;

    auto parsed = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(parsed.ok);

    const auto settings = MakeAdbScanFixtureSettings("127.0.0.1:5554");
    std::vector<AdbDeviceInfo> devices;
    devices.push_back(MakeScanDevice("127.0.0.1:5554"));

    auto compile =
        CompileMaapi(settings, parsed.catalog, "execution", &devices);
    ASSERT_TRUE(compile.ok);
    ASSERT_TRUE(compile.execution_input.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params->adb.has_value());
    const auto& adb =
        *compile.execution_input->maapi.controller_init_params->adb;
    EXPECT_EQ(adb.address, "127.0.0.1:5554");
    EXPECT_EQ(adb.adb_path, "/scan/adb");
    EXPECT_EQ(adb.screencap_method, 0x11u);
    EXPECT_EQ(adb.input_method, 0x22u);
    EXPECT_EQ(adb.config_json, R"({"scan":true})");
}

// preview + 含匹配设备的设备表 → 零设备动作：六字段不填（保持 raw_json
// 值），无任何 adb-device 诊断；preview 编译本身 ok（离线可过）。
TEST(MaapiCompileAdbDeviceScanTest, PreviewTakesNoDeviceAction)
{
    using namespace Das::Plugins::DasMaaPi;

    auto parsed = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(parsed.ok);

    const auto settings = MakeAdbScanFixtureSettings("127.0.0.1:5554");
    std::vector<AdbDeviceInfo> devices;
    devices.push_back(MakeScanDevice("127.0.0.1:5554"));

    auto compile = CompileMaapi(settings, parsed.catalog, "preview", &devices);
    ASSERT_TRUE(compile.ok);
    ASSERT_TRUE(compile.execution_input.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params->adb.has_value());
    const auto& adb =
        *compile.execution_input->maapi.controller_init_params->adb;
    // raw_json 既有值：fixture address=127.0.0.1:5555、adbPath 缺省 "adb"。
    EXPECT_EQ(adb.address, "127.0.0.1:5555");
    EXPECT_EQ(adb.adb_path, "adb");
    EXPECT_EQ(adb.screencap_method, 0u);
    EXPECT_EQ(adb.input_method, 0u);
    EXPECT_EQ(adb.config_json, "{}");
    for (const auto& diag : compile.diagnostics)
    {
        EXPECT_TRUE(
            diag.code != "adb-device-missing"
            && diag.code != "adb-device-ambiguous"
            && diag.code != "adb-device-cache-miss")
            << "unexpected device diagnostic: " << diag.code;
    }
}

// execution + 地址未选择（空）→ 编译不因设备失败，六字段取 raw_json
// （运行期 MaapiRuntime 既有 -10010 路径兜底——既有行为保持）。
TEST(MaapiCompileAdbDeviceScanTest, ExecutionWithoutSelectedAddressUsesRawJson)
{
    using namespace Das::Plugins::DasMaaPi;

    auto parsed = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(parsed.ok);

    const auto                 settings = MakeAdbScanFixtureSettings(nullptr);
    std::vector<AdbDeviceInfo> devices;
    devices.push_back(MakeScanDevice("127.0.0.1:5554"));

    auto compile =
        CompileMaapi(settings, parsed.catalog, "execution", &devices);
    ASSERT_TRUE(compile.ok);
    ASSERT_TRUE(compile.execution_input.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params.has_value());
    ASSERT_TRUE(
        compile.execution_input->maapi.controller_init_params->adb.has_value());
    const auto& adb =
        *compile.execution_input->maapi.controller_init_params->adb;
    EXPECT_EQ(adb.address, "127.0.0.1:5555");
    EXPECT_EQ(adb.adb_path, "adb");
    for (const auto& diag : compile.diagnostics)
    {
        EXPECT_TRUE(
            diag.code != "adb-device-missing"
            && diag.code != "adb-device-ambiguous")
            << "unexpected device diagnostic: " << diag.code;
    }
}

// schedule.kind 投影为根字段（select：candidates off/interval/daily）。
// 字段名走 pi.schedule.* 前缀范式，业务中立。
TEST(MaapiProjectionTest, ProjectSchedule_EmitsKindSelect)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeNeutralProjectionMockCatalog();
    AcceptedSettingsDto settings;
    settings.pi.schedule_kind = "daily";

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    EXPECT_EQ(CountFieldsWithName(document, "schedule.kind"), 1u);
    auto kind_field = FindField(document, "schedule.kind");
    ASSERT_TRUE(kind_field.has_value());
    auto kf = kind_field->as_object();
    ASSERT_TRUE(kf.has_value());
    EXPECT_EQ((*kf)[std::string_view("valueType")].as_sint().value_or(-1), 4);
    EXPECT_EQ((*kf)[std::string_view("inputType")].as_sint().value_or(-1), 1);
    EXPECT_EQ(
        (*kf)[std::string_view("value")].as_string().value_or(""),
        "daily");
    // candidates 含 off/interval/daily 三个 value。
    auto candidates = (*kf)[std::string_view("candidates")].as_array();
    ASSERT_TRUE(candidates.has_value());
    std::set<std::string> candidate_values;
    for (auto&& c : *candidates)
    {
        auto co = c.as_object();
        if (co)
        {
            candidate_values.emplace(
                (*co)[std::string_view("value")].as_string().value_or(""));
        }
    }
    EXPECT_EQ(candidate_values.size(), 3u);
    EXPECT_TRUE(candidate_values.count("off") > 0);
    EXPECT_TRUE(candidate_values.count("interval") > 0);
    EXPECT_TRUE(candidate_values.count("daily") > 0);
}

// kind=daily 投影 dailyTime(inputType=8 time)+dailyCatchUp(bool)。dailyCatchUp
// 缺失默认 true（补跑倾向，服务端按 kind 重投影）。
TEST(MaapiProjectionTest, ProjectSchedule_Daily_EmitsDailyTimeAndCatchUp)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeNeutralProjectionMockCatalog();
    AcceptedSettingsDto settings;
    settings.pi.schedule_kind = "daily";
    settings.pi.schedule_daily_time = "09:30";

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    EXPECT_EQ(CountFieldsWithName(document, "schedule.dailyTime"), 1u);
    auto time_field = FindField(document, "schedule.dailyTime");
    ASSERT_TRUE(time_field.has_value());
    auto tf = time_field->as_object();
    ASSERT_TRUE(tf.has_value());
    EXPECT_EQ((*tf)[std::string_view("valueType")].as_sint().value_or(-1), 4);
    EXPECT_EQ((*tf)[std::string_view("inputType")].as_sint().value_or(-1), 8);
    EXPECT_EQ(
        (*tf)[std::string_view("value")].as_string().value_or(""),
        "09:30");

    EXPECT_EQ(CountFieldsWithName(document, "schedule.dailyCatchUp"), 1u);
    auto catchup_field = FindField(document, "schedule.dailyCatchUp");
    ASSERT_TRUE(catchup_field.has_value());
    auto cf = catchup_field->as_object();
    ASSERT_TRUE(cf.has_value());
    EXPECT_EQ((*cf)[std::string_view("valueType")].as_sint().value_or(-1), 8);
    // 缺失 daily_catch_up → 默认 true（补跑倾向）。
    EXPECT_TRUE((*cf)[std::string_view("value")].as_bool().value_or(false));
}

// kind=interval 只投影 intervalMinutes（valueType=0 int），不发 dailyTime/
// dailyCatchUp（服务端按 kind 重投影，前端零条件可见性）。
TEST(MaapiProjectionTest, ProjectSchedule_Interval_EmitsOnlyIntervalMinutes)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeNeutralProjectionMockCatalog();
    AcceptedSettingsDto settings;
    settings.pi.schedule_kind = "interval";
    settings.pi.schedule_interval_minutes = 90;

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);

    EXPECT_EQ(CountFieldsWithName(document, "schedule.kind"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "schedule.intervalMinutes"), 1u);
    auto interval_field = FindField(document, "schedule.intervalMinutes");
    ASSERT_TRUE(interval_field.has_value());
    auto ifo = interval_field->as_object();
    ASSERT_TRUE(ifo.has_value());
    EXPECT_EQ((*ifo)[std::string_view("valueType")].as_sint().value_or(-1), 0);
    EXPECT_EQ((*ifo)[std::string_view("inputType")].as_sint().value_or(-1), 0);
    EXPECT_EQ((*ifo)[std::string_view("value")].as_sint().value_or(-1), 90);

    // interval kind 不发 daily 字段。
    EXPECT_EQ(CountFieldsWithName(document, "schedule.dailyTime"), 0u);
    EXPECT_EQ(CountFieldsWithName(document, "schedule.dailyCatchUp"), 0u);
}

// kind=off 只投影 schedule.kind，fieldLayout schedule section 只含 kind leaf。
TEST(MaapiProjectionTest, ProjectSchedule_Off_EmitsOnlyKind)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeNeutralProjectionMockCatalog();
    AcceptedSettingsDto settings; // schedule_kind 缺失 = "" = off

    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    auto validate =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(validate.valid) << "error_kind=" << validate.error_kind
                                << " message=" << validate.message;

    EXPECT_EQ(CountFieldsWithName(document, "schedule.kind"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "schedule.intervalMinutes"), 0u);
    EXPECT_EQ(CountFieldsWithName(document, "schedule.dailyTime"), 0u);
    EXPECT_EQ(CountFieldsWithName(document, "schedule.dailyCatchUp"), 0u);

    auto schedule_section = FindLayoutSection(document, "schedule");
    ASSERT_TRUE(schedule_section.has_value());
    auto names = LayoutSectionFieldNames(*schedule_section);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "schedule.kind");
}

// kind 驱动 fieldLayout 重投影：kind=interval → schedule section 只含
// kind+intervalMinutes leaf；kind=daily → kind+dailyTime+dailyCatchUp leaf。
// 前端无客户端条件可见性，靠服务端按 kind 重投影 document。
TEST(MaapiProjectionTest, KindReproject_LayoutMatchesKind)
{
    using namespace Das::Plugins::DasMaaPi;

    auto catalog = MakeNeutralProjectionMockCatalog();

    // interval：schedule section = [kind, intervalMinutes]。
    AcceptedSettingsDto interval_settings;
    interval_settings.pi.schedule_kind = "interval";
    interval_settings.pi.schedule_interval_minutes = 30;
    auto interval_doc =
        ProjectAuthoringDocument(interval_settings, &catalog, {}, 0);
    auto interval_section = FindLayoutSection(interval_doc, "schedule");
    ASSERT_TRUE(interval_section.has_value());
    auto interval_names = LayoutSectionFieldNames(*interval_section);
    EXPECT_EQ(interval_names.size(), 2u);
    EXPECT_NE(
        std::find(
            interval_names.begin(),
            interval_names.end(),
            "schedule.kind"),
        interval_names.end());
    EXPECT_NE(
        std::find(
            interval_names.begin(),
            interval_names.end(),
            "schedule.intervalMinutes"),
        interval_names.end());

    // daily：schedule section = [kind, dailyTime, dailyCatchUp]。
    AcceptedSettingsDto daily_settings;
    daily_settings.pi.schedule_kind = "daily";
    daily_settings.pi.schedule_daily_time = "09:30";
    auto daily_doc = ProjectAuthoringDocument(daily_settings, &catalog, {}, 0);
    auto daily_section = FindLayoutSection(daily_doc, "schedule");
    ASSERT_TRUE(daily_section.has_value());
    auto daily_names = LayoutSectionFieldNames(*daily_section);
    EXPECT_EQ(daily_names.size(), 3u);
    EXPECT_NE(
        std::find(daily_names.begin(), daily_names.end(), "schedule.kind"),
        daily_names.end());
    EXPECT_NE(
        std::find(daily_names.begin(), daily_names.end(), "schedule.dailyTime"),
        daily_names.end());
    EXPECT_NE(
        std::find(
            daily_names.begin(),
            daily_names.end(),
            "schedule.dailyCatchUp"),
        daily_names.end());
}

// setValue target=document fieldName=schedule.kind 路由到
// acceptedProperties.pi.schedule_kind，重投影后 document 含 daily 字段。
TEST(MaapiProjectionTest, SetValue_ScheduleKind_RoutesToAcceptedProperties)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeNeutralProjectionMockCatalog();
    AcceptedSettingsDto settings;

    std::vector<PiDiagnosticDto> diagnostics;
    auto                         payload = Das::Utils::ParseYyjsonFromString(
                                               R"({"target":{"kind":"document"},)"
                                               R"("fieldName":"schedule.kind",)"
                                               R"("value":"daily"})")
                                               .value_or(yyjson::value(nullptr));
    const bool                   ok =
        ApplyStrictSetValueChange(settings, payload, &catalog, diagnostics);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(diagnostics.empty());

    ASSERT_TRUE(settings.pi.schedule_kind.has_value());
    EXPECT_EQ(*settings.pi.schedule_kind, "daily");

    // 重投影：kind=daily document 含 dailyTime/dailyCatchUp 字段。
    auto document = ProjectAuthoringDocument(settings, &catalog, {}, 0);
    EXPECT_EQ(CountFieldsWithName(document, "schedule.kind"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "schedule.dailyTime"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "schedule.dailyCatchUp"), 1u);
    EXPECT_EQ(CountFieldsWithName(document, "schedule.intervalMinutes"), 0u);
}

// setValue target=document fieldName=adbDevice 路由到
// acceptedProperties.pi.controller_adb_device（用户选择的设备 address，Adb
// 控制器执行时经 device_cache 匹配六字段的唯一持久化键）。修复前该字段
// 落入 setValueFieldUnknownDocument fallback——UI 的设备 dropdown 选择
// 报「字段对 document 目标未知」且写不进设置。
TEST(MaapiProjectionTest, SetValue_AdbDevice_RoutesToControllerAdbDevice)
{
    using namespace Das::Plugins::DasMaaPi;

    auto                catalog = MakeNeutralProjectionMockCatalog();
    AcceptedSettingsDto settings;

    {
        std::vector<PiDiagnosticDto> diagnostics;
        auto       payload = Das::Utils::ParseYyjsonFromString(
                                 R"({"target":{"kind":"document"},)"
                                 R"("fieldName":"adbDevice",)"
                                 R"("value":"127.0.0.1:5555"})")
                                 .value_or(yyjson::value(nullptr));
        const bool ok =
            ApplyStrictSetValueChange(settings, payload, &catalog, diagnostics);
        EXPECT_TRUE(ok);
        EXPECT_TRUE(diagnostics.empty());
        ASSERT_TRUE(settings.pi.controller_adb_device.has_value());
        EXPECT_EQ(*settings.pi.controller_adb_device, "127.0.0.1:5555");
    }

    // 空串 = 清除选择（回退未选状态），不是错误。
    {
        std::vector<PiDiagnosticDto> diagnostics;
        auto       payload = Das::Utils::ParseYyjsonFromString(
                                 R"({"target":{"kind":"document"},)"
                                 R"("fieldName":"adbDevice",)"
                                 R"("value":""})")
                                 .value_or(yyjson::value(nullptr));
        const bool ok =
            ApplyStrictSetValueChange(settings, payload, &catalog, diagnostics);
        EXPECT_TRUE(ok);
        EXPECT_TRUE(diagnostics.empty());
        EXPECT_FALSE(settings.pi.controller_adb_device.has_value());
    }

    // 非字符串拒绝并报 error 诊断。
    {
        std::vector<PiDiagnosticDto> diagnostics;
        auto       payload = Das::Utils::ParseYyjsonFromString(
                                 R"({"target":{"kind":"document"},)"
                                 R"("fieldName":"adbDevice",)"
                                 R"("value":42})")
                                 .value_or(yyjson::value(nullptr));
        const bool ok =
            ApplyStrictSetValueChange(settings, payload, &catalog, diagnostics);
        EXPECT_FALSE(ok);
        ASSERT_FALSE(diagnostics.empty());
        EXPECT_EQ(diagnostics.front().severity, "error");
    }
}

// schedule 持久化 round-trip：SerializeAcceptedSettings → ParseAcceptedSettings
// 必须保留 pi.schedule_* 4 字段（apply 落盘后重启 GetAuthoringDocument 走的正是
// 这条 round-trip）。3 个用例分别覆盖 daily/全 off/interval，锁定 engaged-only
// 契约——off 时序列化结果 pi 对象不含 schedule* 键。
TEST(MaapiAuthoringScheduleRoundTrip, DailyKeepsFourFieldsAndIntervalIsNullOpt)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto settings;
    settings.pi.schedule_kind = "daily";
    settings.pi.schedule_daily_time = "09:00";
    settings.pi.schedule_daily_catch_up = true;
    // schedule_interval_minutes 保持 nullopt（daily 不发 interval）。

    auto serialized = SerializeAcceptedSettings(settings);
    auto s_obj = serialized.as_object();
    ASSERT_TRUE(s_obj.has_value());
    auto pi_obj = (*s_obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi_obj.has_value());
    // engaged 才写契约：4 字段中 engaged 的 3 个存在，interval 不存在。
    EXPECT_TRUE(pi_obj->contains(std::string_view("scheduleKind")));
    EXPECT_TRUE(pi_obj->contains(std::string_view("scheduleDailyTime")));
    EXPECT_TRUE(pi_obj->contains(std::string_view("scheduleDailyCatchUp")));
    EXPECT_FALSE(pi_obj->contains(std::string_view("scheduleIntervalMinutes")));

    auto restored = ParseAcceptedSettings(serialized);
    EXPECT_EQ(restored.pi.schedule_kind.value_or(""), "daily");
    EXPECT_EQ(restored.pi.schedule_daily_time.value_or(""), "09:00");
    EXPECT_TRUE(restored.pi.schedule_daily_catch_up.has_value());
    EXPECT_EQ(*restored.pi.schedule_daily_catch_up, true);
    EXPECT_FALSE(restored.pi.schedule_interval_minutes.has_value());
}

TEST(MaapiAuthoringScheduleRoundTrip, AllOffOmitsAllScheduleKeys)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto settings; // 全部 schedule_* = nullopt = off

    auto serialized = SerializeAcceptedSettings(settings);
    auto s_obj = serialized.as_object();
    ASSERT_TRUE(s_obj.has_value());
    auto pi_obj = (*s_obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi_obj.has_value());
    // engaged-only 契约硬断言：off 时 pi 对象不含任何 schedule* 键。
    EXPECT_FALSE(pi_obj->contains(std::string_view("scheduleKind")));
    EXPECT_FALSE(pi_obj->contains(std::string_view("scheduleIntervalMinutes")));
    EXPECT_FALSE(pi_obj->contains(std::string_view("scheduleDailyTime")));
    EXPECT_FALSE(pi_obj->contains(std::string_view("scheduleDailyCatchUp")));

    auto restored = ParseAcceptedSettings(serialized);
    EXPECT_FALSE(restored.pi.schedule_kind.has_value());
    EXPECT_FALSE(restored.pi.schedule_interval_minutes.has_value());
    EXPECT_FALSE(restored.pi.schedule_daily_time.has_value());
    EXPECT_FALSE(restored.pi.schedule_daily_catch_up.has_value());
}

TEST(MaapiAuthoringScheduleRoundTrip, IntervalKeepsKindAndMinutes)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto settings;
    settings.pi.schedule_kind = "interval";
    settings.pi.schedule_interval_minutes = 120;
    // schedule_daily_time / schedule_daily_catch_up 保持 nullopt。

    auto serialized = SerializeAcceptedSettings(settings);
    auto s_obj = serialized.as_object();
    ASSERT_TRUE(s_obj.has_value());
    auto pi_obj = (*s_obj)[std::string_view("pi")].as_object();
    ASSERT_TRUE(pi_obj.has_value());
    EXPECT_TRUE(pi_obj->contains(std::string_view("scheduleKind")));
    EXPECT_TRUE(pi_obj->contains(std::string_view("scheduleIntervalMinutes")));
    EXPECT_FALSE(pi_obj->contains(std::string_view("scheduleDailyTime")));
    EXPECT_FALSE(pi_obj->contains(std::string_view("scheduleDailyCatchUp")));

    auto restored = ParseAcceptedSettings(serialized);
    EXPECT_EQ(restored.pi.schedule_kind.value_or(""), "interval");
    EXPECT_TRUE(restored.pi.schedule_interval_minutes.has_value());
    EXPECT_EQ(*restored.pi.schedule_interval_minutes, 120);
    EXPECT_FALSE(restored.pi.schedule_daily_time.has_value());
    EXPECT_FALSE(restored.pi.schedule_daily_catch_up.has_value());
}
