#include "../src/PretaskRunner.h"
#include "FakeMaaApiBoundary.h"

#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/PiCompiler.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <deque>
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
            return CreateSessionWithContext(nullptr);
        }

        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession>
        CreateSessionWithContext(Das::ExportInterface::IDasJson* context)
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
                    context,
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

// ---------------------------------------------------------------------------
// v2.8.1 end-to-end coverage (pretask execution / hotkey tolerance /
// env snapshot 8 fields / Win32 typed controller dispatch).
//
// These tests drive the REAL DasMaaPi plugin through the PluginManager +
// IDasTaskAuthoringSession pipeline (parse → project → ApplyChange → Compile)
// and then invoke MaaRuntime::Run directly with FakeMaaApiBoundary +
// FakePretaskRunner. The pretask runner seam lets the tests script pretask
// outcomes (zero / nonzero / spawn failure) without spawning real subprocess,
// matching the contract locked in by DasMaaPiRuntime pretask stage.
// ---------------------------------------------------------------------------

namespace
{
    class FakePretaskRunner final : public IPretaskRunner
    {
    public:
        std::vector<PretaskLaunchRequest> launches;
        std::deque<PretaskRunResult>      scripted_results;

        PretaskRunResult Run(const PretaskLaunchRequest& request) override
        {
            launches.push_back(request);
            if (!scripted_results.empty())
            {
                auto result = scripted_results.front();
                scripted_results.pop_front();
                return result;
            }
            PretaskRunResult ok;
            ok.launched = true;
            ok.exit_code = 0;
            return ok;
        }
    };

    class ScopedPretaskRunner final
    {
    public:
        explicit ScopedPretaskRunner(IPretaskRunner* runner)
        {
            SetPretaskRunnerForTest(runner);
        }
        ~ScopedPretaskRunner() { SetPretaskRunnerForTest(nullptr); }
    };

    struct CompiledEnvelope
    {
        CompileResultDto                    compile;
        std::optional<ExecutionEnvelopeDto> envelope;
    };

    // Apply interfacePath setValue + Compile({"purpose":"execution"}) and
    // return the unwrapped compile result + parsed envelope. Mirrors the unwrap
    // pattern from MaapiCompileFixture but drives it through the integration
    // session.
    CompiledEnvelope CompileFixtureForExecution(
        Das::PluginInterface::IDasTaskAuthoringSession* session,
        std::string_view                                fixture_name)
    {
        CompiledEnvelope out;
        const auto interface_path = FixturePath(fixture_name).generic_string();
        auto       set_path = ParseDasJson(
            "{\"baseRevision\":0,\"kind\":\"setValue\",\"payload\":{"
            "\"target\":{\"kind\":\"document\"},\"fieldName\":"
            "\"adapter.interfacePath\",\"value\":\""
            + interface_path + "\"}}");
        DasPtr<Das::ExportInterface::IDasJson> apply_json;
        EXPECT_EQ(
            session->ApplyChange(set_path.Get(), apply_json.Put()),
            DAS_S_OK);
        auto exec_request = ParseDasJson(R"({"purpose":"execution"})");
        DasPtr<Das::ExportInterface::IDasJson> exec_json;
        EXPECT_EQ(
            session->Compile(exec_request.Get(), exec_json.Put()),
            DAS_S_OK);
        auto root = ReadJsonInterface(exec_json.Get()).as_object();
        EXPECT_TRUE(root.has_value());
        if (!root || !root->contains(std::string_view("compile")))
        {
            return out;
        }
        auto compile_obj = (*root)[std::string_view("compile")].as_object();
        if (!compile_obj)
        {
            return out;
        }
        out.compile = yyjson::cast<CompileResultDto>(*compile_obj);
        if (!compile_obj->contains(std::string_view("executionInput")))
        {
            return out;
        }
        auto parsed = ParseExecutionEnvelope(
            (*compile_obj)[std::string_view("executionInput")]);
        if (parsed.result == DAS_S_OK)
        {
            out.envelope = std::move(parsed.envelope);
        }
        return out;
    }
} // namespace

// Pretask success path: PiCompiler fills envelope.pretasks with the 3 pretasks
// declared across interface_v28.jsonc (Bootstrap) + its import
// (Warmup, Cleanup). FakePretaskRunner returns zero exit for each. MaaRuntime
// must run all of them in order, then proceed to CreateController + PostTask.
TEST_F(MaapiIntegrationFixture, V28E2EPretaskSuccessPathRunsBeforeController)
{
    auto session = CreateSession();
    auto compiled =
        CompileFixtureForExecution(session.Get(), "interface_v28.jsonc");
    ASSERT_TRUE(compiled.envelope.has_value());
    // Bootstrap (controller=Android) is included; Warmup/Cleanup have no
    // controller filter so ContainsName(empty, Android)==true includes them.
    // Import merge adds them after main's Bootstrap in source order.
    ASSERT_EQ(compiled.envelope->maapi.pretasks.size(), 3u);
    EXPECT_EQ(compiled.envelope->maapi.pretasks[0].name, "Bootstrap");
    EXPECT_EQ(compiled.envelope->maapi.pretasks[1].name, "Warmup");
    EXPECT_EQ(compiled.envelope->maapi.pretasks[2].name, "Cleanup");

    FakePretaskRunner   runner;
    ScopedPretaskRunner hook(&runner);
    FakeMaaApiBoundary  fake;
    fake.resource_hash = compiled.envelope->maapi.resource_hash.value_or("");
    auto run_result = MaaRuntime::Run(*compiled.envelope, fake, nullptr);
    EXPECT_EQ(run_result.das_result, DAS_S_OK);
    ASSERT_EQ(runner.launches.size(), 3u);
    // Bootstrap is declared with args ["--init","--quiet"]; PiCompiler appends
    // the option JSON only when pretask.option has user-selected values, and
    // the integration test does not set pretask.bootstrap-mode, so the trailing
    // JSON arg is omitted and the original args pass through unchanged.
    EXPECT_EQ(runner.launches[0].exec, "tools/bootstrap.exe");
    EXPECT_EQ(runner.launches[1].exec, "tools/warmup.exe");
    EXPECT_EQ(runner.launches[2].exec, "tools/cleanup.exe");
    EXPECT_EQ(
        runner.launches[0].working_directory.string(),
        compiled.envelope->maapi.interface_directory);
    // Controller + tasker still created after pretask stage completes.
    EXPECT_TRUE(fake.Contains("CreateControllerFromParams:Android:Adb"));
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartDaily:"));
}

// Pretask nonzero exit aborts the run before CreateController. The structured
// diagnostic code is `pretask-nonzero-exit` and provider_code carries the
// raw exit status so operators can locate the failing bootstrap step.
TEST_F(MaapiIntegrationFixture, V28E2EPretaskNonzeroExitAbortsBeforeController)
{
    auto session = CreateSession();
    auto compiled =
        CompileFixtureForExecution(session.Get(), "interface_v28.jsonc");
    ASSERT_TRUE(compiled.envelope.has_value());

    FakePretaskRunner runner;
    PretaskRunResult  nonzero;
    nonzero.launched = true;
    nonzero.exit_code = 7;
    runner.scripted_results.push_back(nonzero);
    ScopedPretaskRunner hook(&runner);
    FakeMaaApiBoundary  fake;
    fake.resource_hash = compiled.envelope->maapi.resource_hash.value_or("");
    auto run_result = MaaRuntime::Run(*compiled.envelope, fake, nullptr);
    EXPECT_EQ(run_result.das_result, DAS_E_MAAPI_PRETASK_FAILED);
    ASSERT_FALSE(run_result.diagnostics.empty());
    EXPECT_EQ(run_result.diagnostics.back().code, "pretask-nonzero-exit");
    ASSERT_TRUE(run_result.diagnostics.back().provider_code.has_value());
    EXPECT_EQ(*run_result.diagnostics.back().provider_code, 7);
    ASSERT_EQ(runner.launches.size(), 1u);
    EXPECT_FALSE(fake.Contains("CreateControllerFromParams:Android:Adb"));
    EXPECT_FALSE(fake.ContainsSubstring("PostTask:"));
}

// Pretask spawn failure (launch=false) surfaces as `pretask-failed` diagnostic
// with the OS error message, aborting before CreateController.
TEST_F(MaapiIntegrationFixture, V28E2EPretaskSpawnFailureAbortsBeforeController)
{
    auto session = CreateSession();
    auto compiled =
        CompileFixtureForExecution(session.Get(), "interface_v28.jsonc");
    ASSERT_TRUE(compiled.envelope.has_value());

    FakePretaskRunner runner;
    PretaskRunResult  spawn_fail;
    spawn_fail.launched = false;
    spawn_fail.failure_message = "ENOENT: tools/bootstrap.exe";
    runner.scripted_results.push_back(spawn_fail);
    ScopedPretaskRunner hook(&runner);
    FakeMaaApiBoundary  fake;
    fake.resource_hash = compiled.envelope->maapi.resource_hash.value_or("");
    auto run_result = MaaRuntime::Run(*compiled.envelope, fake, nullptr);
    EXPECT_EQ(run_result.das_result, DAS_E_MAAPI_PRETASK_FAILED);
    ASSERT_FALSE(run_result.diagnostics.empty());
    EXPECT_EQ(run_result.diagnostics.back().code, "pretask-failed");
    EXPECT_NE(
        run_result.diagnostics.back().message.find("ENOENT"),
        std::string::npos);
    EXPECT_FALSE(fake.Contains("CreateControllerFromParams:Android:Adb"));
}

// Hotkey option tolerance: the parser stores the unknown type as a raw string
// and PiCompiler never references the unreferenced option, so compile + run
// must succeed without crashing. Locks the v2.8.1 hotkey deferred behavior
// (no support, no guard, no crash).
TEST_F(MaapiIntegrationFixture, V28E2EHotkeyOptionToleratedByParserAndCompiler)
{
    auto session = CreateSession();
    auto compiled =
        CompileFixtureForExecution(session.Get(), "interface_v28_hotkey.jsonc");
    ASSERT_TRUE(compiled.envelope.has_value());
    EXPECT_TRUE(compiled.compile.ok)
        << "hotkey option in fixture must not break compile";
    // Sanity: the hotkey fixture still has the Android controller + DailyFarm
    // task, so the run reaches PostTask via the legacy single-arg path.
    FakeMaaApiBoundary fake;
    fake.resource_hash = compiled.envelope->maapi.resource_hash.value_or("");
    auto run_result = MaaRuntime::Run(*compiled.envelope, fake, nullptr);
    EXPECT_EQ(run_result.das_result, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartDaily:"));
}

// Env snapshot 8 fields: PiCompiler must populate all 8 PI_* env variables
// (D-86-12). compile→envelope→run path needs every field non-empty so the
// production agent runtime can emit PI_CLIENT_VERSION / PI_CLIENT_MAAFW_VERSION
// alongside the original 6.
TEST_F(MaapiIntegrationFixture, V28E2EEnvSnapshotHasAllEightPiFields)
{
    auto session = CreateSession();
    auto compiled =
        CompileFixtureForExecution(session.Get(), "interface_v28.jsonc");
    ASSERT_TRUE(compiled.envelope.has_value());
    const auto& env = compiled.envelope->maapi.pi_env;
    EXPECT_FALSE(env.interface_version.empty());
    EXPECT_FALSE(env.client_name.empty());
    EXPECT_FALSE(env.client_version.empty())
        << "client_version must be filled from kPluginVersionText";
    EXPECT_FALSE(env.client_language.empty());
    EXPECT_FALSE(env.client_maafw_version.empty())
        << "client_maafw_version must be filled from MaaVersion()";
    EXPECT_FALSE(env.project_version.empty());
    EXPECT_FALSE(env.controller_json.empty());
    EXPECT_FALSE(env.resource_json.empty());
}

// Win32 typed controller dispatch: PiCompiler must emit
// controller_init_params.{controller_type=Win32, win32=...} for the Desktop
// controller in interface_controller.jsonc, and MaaRuntime::Run routes through
// CreateControllerFromParams (not the single-arg legacy overload). This is the
// E2E path that the boost::dll dynamic loader ( ScopedMaaApiBoundary ) would
// take in production; here we use FakeMaaApiBoundary to verify the envelope
// shape + dispatch routing end-to-end through the real plugin compile path.
TEST_F(MaapiIntegrationFixture, V28E2EWin32TypedControllerEmitsTypedInitParams)
{
    const auto interface_path =
        FixturePath("interface_controller.jsonc").generic_string();
    // Select the Desktop (Win32) controller explicitly so PiCompiler emits the
    // typed init params instead of defaulting to the first controller
    // (Android).
    auto context = ParseDasJson(
        "{\"adapter\":{\"interfacePath\":\"" + interface_path
        + "\",\"executionPolicy\":{\"failFast\":true}},"
          "\"pi\":{\"controllerName\":\"Desktop\","
          "\"resourceName\":\"Official\"}}");
    auto session = CreateSessionWithContext(context.Get());

    auto exec_request = ParseDasJson(R"({"purpose":"execution"})");
    DasPtr<Das::ExportInterface::IDasJson> exec_json;
    ASSERT_EQ(session->Compile(exec_request.Get(), exec_json.Put()), DAS_S_OK);
    auto root = ReadJsonInterface(exec_json.Get()).as_object();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(root->contains(std::string_view("compile")));
    auto compile_obj = (*root)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile_obj.has_value());
    ASSERT_TRUE(compile_obj->contains(std::string_view("executionInput")));
    auto parsed = ParseExecutionEnvelope(
        (*compile_obj)[std::string_view("executionInput")]);
    ASSERT_EQ(parsed.result, DAS_S_OK) << "message: " << parsed.message;
    ASSERT_TRUE(parsed.envelope.maapi.controller_init_params.has_value());
    const auto& params = *parsed.envelope.maapi.controller_init_params;
    EXPECT_EQ(params.controller_type, "Win32");
    ASSERT_TRUE(params.win32.has_value());
    EXPECT_EQ(params.win32->class_regex, "Arknights.*");
    EXPECT_EQ(params.win32->window_regex, ".*AutoScript");
    // Foreground = DXGI_DesktopDup_Window | ScreenDC (bit 3 | bit 5 = 40),
    // per MaaFramework MaaDef.h Win32ScreencapMethod enum and the
    // ControllerMethodMaps predefined-combo table.
    EXPECT_EQ(params.win32->screencap_method, 40ULL);
    // display_* options are applied inside ScopedMaaApiBoundary in production;
    // the E2E envelope carries them so the boundary can consume the values.
    ASSERT_TRUE(params.display_short_side.has_value());
    EXPECT_EQ(*params.display_short_side, 720);
    ASSERT_TRUE(params.display_long_side.has_value());
    EXPECT_EQ(*params.display_long_side, 1280);

    FakeMaaApiBoundary fake;
    fake.resource_hash = parsed.envelope.maapi.resource_hash.value_or("");
    auto run_result = MaaRuntime::Run(parsed.envelope, fake, nullptr);
    EXPECT_EQ(run_result.das_result, DAS_S_OK);
    EXPECT_TRUE(fake.Contains("CreateControllerFromParams:Desktop:Win32"));
    ASSERT_TRUE(fake.last_controller_init_params.has_value());
    EXPECT_EQ(fake.last_controller_init_params->controller_type, "Win32");
    // Legacy single-arg overload must NOT have been invoked for typed
    // controllers (the method is deleted — only its name is recorded here as
    // a regression guard against accidental reintroduction).
    EXPECT_FALSE(fake.Contains("CreateController:Desktop:Win32"));
}
