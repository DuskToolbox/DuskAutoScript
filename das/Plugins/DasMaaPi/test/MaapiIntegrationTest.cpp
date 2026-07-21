#include "FakeMaaApiBoundary.h"

#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

    std::filesystem::path PluginRootDir()
    {
        return std::filesystem::path{IpcTestConfig::GetPluginDir()};
    }

    std::filesystem::path PluginPackageDir()
    {
        return PluginRootDir() / "DasMaaPi";
    }

    std::filesystem::path FixturePath(std::string_view name)
    {
        return std::filesystem::current_path() / "DasMaaPi" / "test"
               / "fixtures" / std::filesystem::path{name};
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
        std::unique_ptr<PluginManager> plugin_manager_;
        std::shared_ptr<Das::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
        std::thread ipc_run_thread_;
    };

} // namespace

TEST_F(MaapiIntegrationFixture, SampleInterfaceAuthoringCompileRuntimeDryRun)
{
    auto session = CreateSession();

    DasPtr<Das::ExportInterface::IDasJson> initial_document_json;
    ASSERT_EQ(
        session->GetDocument(nullptr, initial_document_json.Put()),
        DAS_S_OK);
    auto initial_envelope = ReadJsonInterface(initial_document_json.Get());
    auto initial_env_obj = initial_envelope.as_object();
    ASSERT_TRUE(initial_env_obj.has_value());
    EXPECT_TRUE(initial_env_obj->contains(std::string_view("document")));
    EXPECT_TRUE(initial_env_obj->contains(std::string_view("diagnostics")));
    auto initial_document = (*initial_env_obj)[std::string_view("document")];
    // D-84-02: 严格 wire 下 document 用 viewKind 而不是 kind。
    EXPECT_EQ(
        (*initial_document.as_object())[std::string_view("viewKind")]
            .as_string()
            .value_or(""),
        std::string_view("formSequence"));
    EXPECT_FALSE(
        initial_document.as_object()->contains(std::string_view("kind")));

    auto interface_path =
        FixturePath("sample_interface.jsonc").generic_string();
    auto change = ParseDasJson(
        "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{\"target\":{"
        "\"kind\":\"document\"},\"fieldName\":\"adapter.interfacePath\","
        "\"value\":\""
        + interface_path + "\"}}");
    DasPtr<Das::ExportInterface::IDasJson> apply_json;
    ASSERT_EQ(session->ApplyChange(change.Get(), apply_json.Put()), DAS_S_OK);
    auto apply = ReadJsonInterface(apply_json.Get());
    auto apply_obj = apply.as_object();
    ASSERT_TRUE(apply_obj.has_value());
    // D-84-04/08: 严格 Apply envelope = {acceptedProperties, document,
    // diagnostics}。84-06 删除了 ProjectAuthoringDocument 从 catalog 派生
    // sourceFingerprint 的逻辑，envelope 根不再携带 sourceFingerprint /
    // migration；acceptedProperties.adapter.sourceFingerprint 只反映 input
    // 端写入值，interfacePath setValue 不再回灌指纹。验证 apply 拾取新接口
    // 的可观测信号转为：acceptedProperties.adapter.interfacePath 与
    // document.actionCatalog.entries[]。
    EXPECT_FALSE(apply_obj->contains(std::string_view("sourceFingerprint")));
    EXPECT_FALSE(apply_obj->contains(std::string_view("migration")));
    auto accepted =
        (*apply_obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    auto adapter = (*accepted)[std::string_view("adapter")].as_object();
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(
        (*adapter)[std::string_view("interfacePath")].as_string().value_or(""),
        interface_path);
    auto document = (*apply_obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    // D-84-02/13/31: catalog 不再位于 document 根（MaaPI 私有候选 dump
    // 已删除）。新 IR 把 PI tasks 投影到 actionCatalog.entries[]，
    // actionId = task.dto.name；sample_interface.jsonc 的 task name=Daily。
    ASSERT_FALSE(document->contains(std::string_view("catalog")));
    auto action_catalog =
        (*document)[std::string_view("actionCatalog")].as_object();
    ASSERT_TRUE(action_catalog.has_value());
    auto entries = (*action_catalog)[std::string_view("entries")].as_array();
    ASSERT_TRUE(entries.has_value());
    bool found_daily_action = false;
    for (auto it = entries->begin(); it != entries->end(); ++it)
    {
        auto entry = it->as_object();
        if (!entry)
        {
            continue;
        }
        if ((*entry)[std::string_view("actionId")].as_string().value_or("")
            == "Daily")
        {
            found_daily_action = true;
            break;
        }
    }
    EXPECT_TRUE(found_daily_action);

    // D-84-08: Compile 严格 envelope = {compile, diagnostics[]}。PiCompiler
    // 的 ok/canExecute/executionInput 位于 compile 子对象；测试 unwrap 后
    // 断言。
    auto preview_request = ParseDasJson(R"({"purpose":"preview"})");
    DasPtr<Das::ExportInterface::IDasJson> preview_json;
    ASSERT_EQ(
        session->Compile(preview_request.Get(), preview_json.Put()),
        DAS_S_OK);
    auto preview_env = ReadJsonInterface(preview_json.Get());
    auto preview_env_obj = preview_env.as_object();
    ASSERT_TRUE(preview_env_obj.has_value());
    ASSERT_TRUE(preview_env_obj->contains(std::string_view("compile")));
    auto preview_opt =
        (*preview_env_obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(preview_opt.has_value());
    auto preview = *preview_opt;
    EXPECT_TRUE(
        preview[std::string_view("canExecute")].as_bool().value_or(false));
    EXPECT_FALSE(preview.contains(std::string_view("executionInput")));

    auto execution_request = ParseDasJson(R"({"purpose":"execution"})");
    DasPtr<Das::ExportInterface::IDasJson> execution_json;
    ASSERT_EQ(
        session->Compile(execution_request.Get(), execution_json.Put()),
        DAS_S_OK);
    auto execution_env = ReadJsonInterface(execution_json.Get());
    auto execution_env_obj = execution_env.as_object();
    ASSERT_TRUE(execution_env_obj.has_value());
    ASSERT_TRUE(execution_env_obj->contains(std::string_view("compile")));
    auto execution_compile_opt =
        (*execution_env_obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(execution_compile_opt.has_value());
    auto execution_compile = *execution_compile_opt;
    ASSERT_TRUE(execution_compile.contains(std::string_view("executionInput")));
    auto envelope_value = execution_compile[std::string_view("executionInput")];
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
