#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/Plugins/DasMaaPi/PiCompiler.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <gtest/gtest.h>

#include "I18nResolver.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

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

    std::filesystem::path UniqueSettingsDir()
    {
        const auto ticks =
            std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path()
               / ("das_maapi_compile_" + std::to_string(ticks));
    }

    class MaapiCompileFixture : public ::testing::Test
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
            std::filesystem::remove_all(settings_dir_);
        }

        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> CreateSession(
            std::string context)
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

        yyjson::value Compile(std::string context, std::string purpose)
        {
            auto session = CreateSession(std::move(context));
            // D-84-08: Compile 严格 envelope = {compile, diagnostics}。purpose
            // 是 Provider 私有 preview/execution 模式标志，PiCompiler 读取后
            // 决定是否 emit executionInput。测试断言历来读 PiCompiler 输出
            // （ok/canExecute/executionInput/...），故这里 unwrap compile
            // 字段。
            auto request = ParseDasJson(R"({"purpose":")" + purpose + "\"}");
            DasPtr<Das::ExportInterface::IDasJson> result_json;
            EXPECT_EQ(
                session->Compile(request.Get(), result_json.Put()),
                DAS_S_OK);
            auto envelope = ReadJsonInterface(result_json.Get());
            // 84-07 严格 envelope = {compile, diagnostics}；测试历史断言直接
            // 针对 PiCompiler 输出（ok/canExecute/executionInput/...），故
            // 这里 unwrap compile 字段。
            auto env_obj = envelope.as_object();
            if (env_obj && env_obj->contains(std::string_view("compile")))
            {
                return Das::Utils::CloneYyjsonValue(
                    (*env_obj)[std::string_view("compile")]);
            }
            return envelope;
        }

        std::filesystem::path settings_dir_;
        std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                       settings_manager_;
        std::unique_ptr<PluginManager> plugin_manager_;
        std::shared_ptr<Das::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
        std::thread ipc_run_thread_;
    };

    std::string CompileContext(std::string_view path)
    {
        return R"({"adapter":{"interfacePath":")" + std::string(path)
               + "\",\"executionPolicy\":{\"failFast\":true}},\"pi\":{"
                 "\"controllerName\":\"Android\","
                 "\"resourceName\":\"Official\","
                 "\"globalOptions\":[{\"optionName\":\"difficulty\","
                 "\"kind\":\"select\",\"selectedCases\":[\"hard\"]}],"
                 "\"resourceOptions\":[{\"optionName\":\"server\","
                 "\"kind\":\"switch\",\"boolValue\":true}],"
                 "\"controllerOptions\":[{\"optionName\":\"controller-mode\","
                 "\"kind\":\"select\",\"selectedCases\":[\"safe\"]},"
                 "{\"optionName\":\"inactive\",\"kind\":\"select\","
                 "\"selectedCases\":[\"skip\"]}],"
                 "\"tasks\":[{\"taskName\":\"DailyFarm\",\"enabled\":true,"
                 "\"options\":[{\"optionName\":\"retry\",\"kind\":\"input\","
                 "\"inputValues\":{\"times\":\"7\"}},"
                 "{\"optionName\":\"medicine\",\"kind\":\"checkbox\","
                 "\"selectedCases\":[\"large\",\"small\"]},"
                 "{\"optionName\":\"notify\",\"kind\":\"switch\","
                 "\"boolValue\":true}]}]}}";
    }

    // Variant of CompileContext whose selected option names match the
    // interface_v26_jsonc.jsonc fixture (stage / use-medicine /
    // controller-mode), not interface_compile.jsonc (difficulty / medicine /
    // inactive). Reusing CompileContext against the v26 fixture yields
    // "missing-option" errors and canExecute=false.
    std::string CompileContextV26(std::string_view path)
    {
        return R"({"adapter":{"interfacePath":")" + std::string(path)
               + "\",\"executionPolicy\":{\"failFast\":true}},\"pi\":{"
                 "\"controllerName\":\"Android\","
                 "\"resourceName\":\"Official\","
                 "\"globalOptions\":[{\"optionName\":\"stage\","
                 "\"kind\":\"select\",\"selectedCases\":[\"1-1\"]}],"
                 "\"resourceOptions\":[{\"optionName\":\"server\","
                 "\"kind\":\"switch\",\"boolValue\":true}],"
                 "\"controllerOptions\":[{\"optionName\":\"controller-mode\","
                 "\"kind\":\"select\",\"selectedCases\":[\"safe\"]}],"
                 "\"tasks\":[{\"taskName\":\"DailyFarm\",\"enabled\":true,"
                 "\"options\":[{\"optionName\":\"retry\",\"kind\":\"input\","
                 "\"inputValues\":{\"times\":\"7\"}},"
                 "{\"optionName\":\"use-medicine\",\"kind\":\"checkbox\","
                 "\"selectedCases\":[\"large\",\"small\"]},"
                 "{\"optionName\":\"notify\",\"kind\":\"switch\","
                 "\"boolValue\":true}]}]}}";
    }

    // D-86-04 setting→global merge: drives the augmented
    // interface_compile.jsonc fixture whose setting[] references difficulty
    // (overlap with global_option, dedup), language (setting-only), bonus
    // (setting-only, second partition) and filtered-opt (setting-only, v2.8.1
    // controller mismatch). User selections for setting-referenced options
    // are routed through globalOptions storage.
    std::string CompileContextSetting(std::string_view path)
    {
        return R"({"adapter":{"interfacePath":")" + std::string(path)
               + "\",\"executionPolicy\":{\"failFast\":true}},\"pi\":{"
                 "\"controllerName\":\"Android\","
                 "\"resourceName\":\"Official\","
                 "\"globalOptions\":["
                 "{\"optionName\":\"difficulty\",\"kind\":\"select\","
                 "\"selectedCases\":[\"hard\"]},"
                 "{\"optionName\":\"language\",\"kind\":\"select\","
                 "\"selectedCases\":[\"zh\"]},"
                 "{\"optionName\":\"bonus\",\"kind\":\"select\","
                 "\"selectedCases\":[\"extra\"]},"
                 "{\"optionName\":\"filtered-opt\",\"kind\":\"select\","
                 "\"selectedCases\":[\"skip\"]}"
                 "],"
                 "\"tasks\":[{\"taskName\":\"DailyFarm\",\"enabled\":true}]}}";
    }

    // Variant with NO user option selections: every option stays unedited.
    // Used to verify PiCompiler merges PI defaults (default_case /
    // input.default / switch→"No") into execution.pipeline_override
    // instead of dropping them.
    std::string CompileContextDefaults(std::string_view path)
    {
        return R"({"adapter":{"interfacePath":")" + std::string(path)
               + "\",\"executionPolicy\":{\"failFast\":true}},\"pi\":{"
                 "\"controllerName\":\"Android\","
                 "\"resourceName\":\"Official\","
                 "\"tasks\":[{\"taskName\":\"DailyFarm\",\"enabled\":true}]}}";
    }

    // Variant selecting only `difficulty=hard` at global level; every other
    // option stays unedited. Used to verify an explicit user selection wins
    // over the PI default for the same option key while unrelated options
    // still fall back to their defaults.
    std::string CompileContextSingleGlobal(std::string_view path)
    {
        return R"({"adapter":{"interfacePath":")" + std::string(path)
               + "\",\"executionPolicy\":{\"failFast\":true}},\"pi\":{"
                 "\"controllerName\":\"Android\","
                 "\"resourceName\":\"Official\","
                 "\"globalOptions\":[{\"optionName\":\"difficulty\","
                 "\"kind\":\"select\",\"selectedCases\":[\"hard\"]}],"
                 "\"tasks\":[{\"taskName\":\"DailyFarm\",\"enabled\":true}]}}";
    }
} // namespace

TEST_F(MaapiCompileFixture, CompilePreviewOmitsExecutionEnvelope)
{
    auto result = Compile(
        CompileContext(FixturePath("interface_compile.jsonc").generic_string()),
        "preview");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_TRUE(
        (*obj)[std::string_view("canExecute")].as_bool().value_or(false));
    EXPECT_FALSE(obj->contains(std::string_view("executionInput")));
}

TEST_F(MaapiCompileFixture, CompileExecutionReturnsEnvelope)
{
    auto result = Compile(
        CompileContext(FixturePath("interface_compile.jsonc").generic_string()),
        "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE(obj->contains(std::string_view("executionInput")));
    auto envelope = (*obj)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(envelope.has_value());
    auto maapi = (*envelope)[std::string_view("maapi")].as_object();
    ASSERT_TRUE(maapi.has_value());
    EXPECT_TRUE(
        (*maapi)[std::string_view("failFast")].as_bool().value_or(false));
    EXPECT_EQ(
        (*maapi)[std::string_view("resourceHash")].as_string().value_or(""),
        "hash-expected");
}

TEST_F(
    MaapiCompileFixture,
    CompileExecutionIncludesTypedControllerSpecAndRawPiEnv)
{
    auto result = Compile(
        CompileContext(FixturePath("interface_compile.jsonc").generic_string()),
        "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto envelope = (*obj)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(envelope.has_value());
    auto maapi = (*envelope)[std::string_view("maapi")].as_object();
    ASSERT_TRUE(maapi.has_value());

    auto controller = (*maapi)[std::string_view("controller")].as_object();
    ASSERT_TRUE(controller.has_value());
    EXPECT_EQ(
        (*controller)[std::string_view("name")].as_string().value_or(""),
        "Android");
    EXPECT_EQ(
        (*controller)[std::string_view("type")].as_string().value_or(""),
        "Adb");
    EXPECT_EQ(
        (*controller)[std::string_view("adbPath")].as_string().value_or(""),
        "adb");
    EXPECT_EQ(
        (*controller)[std::string_view("configJson")].as_string().value_or(""),
        "{}");

    auto pi_env = (*maapi)[std::string_view("piEnv")].as_object();
    ASSERT_TRUE(pi_env.has_value());
    const auto raw_controller =
        (*pi_env)[std::string_view("controllerJson")].as_string().value_or("");
    EXPECT_NE(raw_controller.find(R"("name":"Android")"), std::string::npos);
    EXPECT_NE(raw_controller.find(R"("type":"Adb")"), std::string::npos);
}

TEST_F(MaapiCompileFixture, PipelineOverrideMergesOptions)
{
    auto result = Compile(
        CompileContext(FixturePath("interface_compile.jsonc").generic_string()),
        "execution");
    auto maapi = (*(*result.as_object())[std::string_view("executionInput")]
                       .as_object())[std::string_view("maapi")]
                     .as_object();
    auto tasks = (*maapi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    auto first_task = (*tasks)[0].as_object();
    ASSERT_TRUE(first_task.has_value());
    auto pipeline =
        (*first_task)[std::string_view("pipelineOverride")].as_object();
    ASSERT_TRUE(pipeline.has_value());
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Stage")]
              .as_object())[std::string_view("value")]
            .as_string()
            .value_or(""),
        "global-hard");
    EXPECT_TRUE(pipeline->contains(std::string_view("MedicineSmall")));
    EXPECT_TRUE(pipeline->contains(std::string_view("MedicineLarge")));
    EXPECT_TRUE(pipeline->contains(std::string_view("Notify")));
    EXPECT_FALSE(pipeline->contains(std::string_view("Inactive")));
}

TEST_F(MaapiCompileFixture, CompileExecutionEnvSnapshotHasClientVersions)
{
    // D-86-12: PiEnvSnapshotDto 从 6 字段拓宽到 8(补 client_version +
    // client_maafw_version)。compile→envelope→run 路径须让全 8 个 PI_*
    // 环境变量可达；禁止空 stub（两字段必须由 PiCompiler 真实填充）。
    auto result = Compile(
        CompileContext(FixturePath("interface_compile.jsonc").generic_string()),
        "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto envelope = (*obj)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(envelope.has_value());
    auto maapi = (*envelope)[std::string_view("maapi")].as_object();
    ASSERT_TRUE(maapi.has_value());
    auto pi_env = (*maapi)[std::string_view("piEnv")].as_object();
    ASSERT_TRUE(pi_env.has_value());

    const auto client_version =
        (*pi_env)[std::string_view("clientVersion")].as_string().value_or("");
    const auto client_maafw_version =
        (*pi_env)[std::string_view("clientMaafwVersion")].as_string().value_or(
            "");
    EXPECT_FALSE(client_version.empty())
        << "piEnv.clientVersion 必须由 PiCompiler 填充，禁止空 stub";
    EXPECT_FALSE(client_maafw_version.empty())
        << "piEnv.clientMaafwVersion 必须由 PiCompiler 填充(源 MaaVersion())，禁止空 stub";
}

TEST_F(MaapiCompileFixture, AgentBoundaryAllowsExecutionEnvelope)
{
    auto result = Compile(
        CompileContextV26(
            FixturePath("interface_v26_jsonc.jsonc").generic_string()),
        "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE(
        (*obj)[std::string_view("canExecute")].as_bool().value_or(false));
    auto summary = (*obj)[std::string_view("summary")].as_object();
    ASSERT_TRUE(summary.has_value());
    EXPECT_TRUE(
        (*summary)[std::string_view("requiresAgentRuntime")].as_bool().value_or(
            false));
    auto envelope = (*obj)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(envelope.has_value());
    auto maapi = (*envelope)[std::string_view("maapi")].as_object();
    ASSERT_TRUE(maapi.has_value());
    auto agents = (*maapi)[std::string_view("agent")].as_array();
    ASSERT_TRUE(agents.has_value());
    ASSERT_EQ(agents->size(), 1u);
    auto agent = (*agents)[0].as_object();
    ASSERT_TRUE(agent.has_value());
    EXPECT_EQ(
        (*agent)[std::string_view("childExec")].as_string().value_or(""),
        "agent.exe");
}

// D-86-04: setting[].option[] 引用的 option 视为额外 global option，编译期
// 并入 global 级 pipeline_override 合并。
// language 与 bonus 仅由 setting 引用（未列入 global_option[]），其选中值经
// globalOptions 存储路由后应在 task.pipelineOverride 中可见；优先级
// global<resource<controller<task 不变（setting 在 global 段并入）。
TEST_F(MaapiCompileFixture, SettingReferencedOptionMergesAtGlobalLevel)
{
    auto result = Compile(
        CompileContextSetting(
            FixturePath("interface_compile.jsonc").generic_string()),
        "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto envelope = (*obj)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(envelope.has_value());
    auto maapi = (*envelope)[std::string_view("maapi")].as_object();
    ASSERT_TRUE(maapi.has_value());
    auto tasks = (*maapi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    auto first_task = (*tasks)[0].as_object();
    ASSERT_TRUE(first_task.has_value());
    auto pipeline =
        (*first_task)[std::string_view("pipelineOverride")].as_object();
    ASSERT_TRUE(pipeline.has_value());

    // 显式 global_option 仍正常合并。
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Stage")]
              .as_object())[std::string_view("value")]
            .as_string()
            .value_or(""),
        "global-hard");
    // 仅由 setting 引用的 option：language（performance 分区）。
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Language")]
              .as_object())[std::string_view("code")]
            .as_string()
            .value_or(""),
        "zh");
    // 仅由 setting 引用的 option：bonus（extras 分区，证明跨分区收集）。
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Bonus")]
              .as_object())[std::string_view("tier")]
            .as_string()
            .value_or(""),
        "extra");
}

// 同一 option 键同时出现在 global_option[] 与 setting[].option[] → 仅合并一次
// （幂等）。difficulty 在两侧均出现；结果应确定且无 missing-option 重复诊断。
TEST_F(MaapiCompileFixture, SettingOptionDedupedWithExplicitGlobalOption)
{
    auto result = Compile(
        CompileContextSetting(
            FixturePath("interface_compile.jsonc").generic_string()),
        "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    // 无 missing-option / 重复诊断：ok=true。
    EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    auto envelope = (*obj)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(envelope.has_value());
    auto maapi = (*envelope)[std::string_view("maapi")].as_object();
    ASSERT_TRUE(maapi.has_value());
    auto tasks = (*maapi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    auto pipeline =
        (*(*tasks)[0].as_object())[std::string_view("pipelineOverride")]
            .as_object();
    ASSERT_TRUE(pipeline.has_value());
    // 单次合并产生确定值（select 取 selectedCases[0]，重复 MergeObject 不会
    // 改变输出；断言锁定确定值非破坏）。
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Stage")]
              .as_object())[std::string_view("value")]
            .as_string()
            .value_or(""),
        "global-hard");
}

// setting 引用的 option 仍受自身 controller/resource 激活过滤 (IsActive
// v2.8.1)。 filtered-opt 的 controller=["MissingController"] 与当前 Android
// 不匹配 → 合并阶段被跳过，pipelineOverride 不含 Filtered 键。
TEST_F(MaapiCompileFixture, SettingOptionRespectsV281IsActiveFilter)
{
    auto result = Compile(
        CompileContextSetting(
            FixturePath("interface_compile.jsonc").generic_string()),
        "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto envelope = (*obj)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(envelope.has_value());
    auto maapi = (*envelope)[std::string_view("maapi")].as_object();
    ASSERT_TRUE(maapi.has_value());
    auto tasks = (*maapi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    auto pipeline =
        (*(*tasks)[0].as_object())[std::string_view("pipelineOverride")]
            .as_object();
    ASSERT_TRUE(pipeline.has_value());
    EXPECT_FALSE(pipeline->contains(std::string_view("Filtered")));
}

// PI v2.7.0 option default merging: unedited options must contribute their
// PI defaults (default_cases / input.default_value / switch→"No") to
// execution.pipeline_override. Prior to ResolveOptionSelection the compiler
// only merged user-edited options, silently dropping defaults. This test
// drives a context with NO user selections so every option falls back.
TEST_F(MaapiCompileFixture, UneditedOptionDefaultsMergeIntoPipeline)
{
    auto result = Compile(
        CompileContextDefaults(
            FixturePath("interface_compile.jsonc").generic_string()),
        "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false))
        << "compile must succeed when options rely on PI defaults";
    auto envelope = (*obj)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(envelope.has_value());
    auto maapi = (*envelope)[std::string_view("maapi")].as_object();
    ASSERT_TRUE(maapi.has_value());
    auto tasks = (*maapi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    auto pipeline =
        (*(*tasks)[0].as_object())[std::string_view("pipelineOverride")]
            .as_object();
    ASSERT_TRUE(pipeline.has_value());

    // global_option difficulty default_cases=["hard"] →
    // Stage.value=global-hard.
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Stage")]
              .as_object())[std::string_view("value")]
            .as_string()
            .value_or(""),
        "global-hard");
    // setting-only option language default_cases=["zh"] → Language.code=zh
    // (proves setting-referenced defaults also merge).
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Language")]
              .as_object())[std::string_view("code")]
            .as_string()
            .value_or(""),
        "zh");
    // resource option server (switch, unedited) → PI default "No" branch has
    // no pipeline_override, so Server key must be absent.
    EXPECT_FALSE(pipeline->contains(std::string_view("Server")));
    // controller option controller-mode default_cases=["safe"] →
    // Controller.value=safe.
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Controller")]
              .as_object())[std::string_view("value")]
            .as_string()
            .value_or(""),
        "safe");
    // task option retry (input) default_value="5" → Retry.times="5".
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Retry")]
              .as_object())[std::string_view("times")]
            .as_string()
            .value_or(""),
        "5");
    // task option medicine (checkbox) default_cases=["small"] →
    // MedicineSmall only.
    EXPECT_TRUE(pipeline->contains(std::string_view("MedicineSmall")));
    EXPECT_FALSE(pipeline->contains(std::string_view("MedicineLarge")));
    // task option notify (switch, unedited) → default "No" branch has no
    // pipeline_override, Notify key absent.
    EXPECT_FALSE(pipeline->contains(std::string_view("Notify")));
    // filtered-opt is gated by controller=["MissingController"] IsActive
    // filter → skipped even when its default would otherwise merge.
    EXPECT_FALSE(pipeline->contains(std::string_view("Filtered")));
}

// PI v2.7.0 option default merging: switch option whose falsy case name is
// lowercase ("no") and carries a real pipeline_override must contribute that
// override when unedited. The default branch is located by case-insensitive
// No/no/N/n matching (mirroring SwitchCaseName for edited switches), not by
// a hardcoded "No" literal — otherwise lowercase "关" branch overrides are
// silently dropped.
TEST_F(MaapiCompileFixture, UneditedSwitchLowercaseNoCaseMergesOverride)
{
    auto result = Compile(
        CompileContextDefaults(
            FixturePath("interface_compile.jsonc").generic_string()),
        "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false))
        << "compile must succeed when switch options rely on PI defaults";
    auto envelope = (*obj)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(envelope.has_value());
    auto maapi = (*envelope)[std::string_view("maapi")].as_object();
    ASSERT_TRUE(maapi.has_value());
    auto tasks = (*maapi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    auto pipeline =
        (*(*tasks)[0].as_object())[std::string_view("pipelineOverride")]
            .as_object();
    ASSERT_TRUE(pipeline.has_value());

    // notify-lower (switch, unedited) → default falsy case is lowercase "no",
    // whose pipeline_override {NotifyLower:{state:"off"}} must merge.
    ASSERT_TRUE(pipeline->contains(std::string_view("NotifyLower")))
        << "lowercase 'no' case override must merge when switch is unedited";
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("NotifyLower")]
              .as_object())[std::string_view("state")]
            .as_string()
            .value_or(""),
        "off");
}

// Explicit user selection wins over the PI default for the same option key.
// Drives CompileContextSingleGlobal which selects only difficulty=hard at
// global scope; every other option still falls back to defaults.
TEST_F(MaapiCompileFixture, EditedOptionSelectionOverridesDefault)
{
    auto result = Compile(
        CompileContextSingleGlobal(
            FixturePath("interface_compile.jsonc").generic_string()),
        "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    auto envelope = (*obj)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(envelope.has_value());
    auto maapi = (*envelope)[std::string_view("maapi")].as_object();
    ASSERT_TRUE(maapi.has_value());
    auto tasks = (*maapi)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    auto pipeline =
        (*(*tasks)[0].as_object())[std::string_view("pipelineOverride")]
            .as_object();
    ASSERT_TRUE(pipeline.has_value());

    // Explicit global selection difficulty=hard wins (same value as default
    // here, but the merge path is the user-edited branch).
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Stage")]
              .as_object())[std::string_view("value")]
            .as_string()
            .value_or(""),
        "global-hard");
    // Unedited options still fall back to their defaults alongside the edit.
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Controller")]
              .as_object())[std::string_view("value")]
            .as_string()
            .value_or(""),
        "safe");
    EXPECT_EQ(
        (*(*pipeline)[std::string_view("Retry")]
              .as_object())[std::string_view("times")]
            .as_string()
            .value_or(""),
        "5");
}

// PI interface evolution may drop an option from the catalog while stored
// user settings still references its key. Compile must NOT fail (forward-
// progress contract) but must emit a non-blocking warning diagnostic so
// operators can clean up settings.json.
TEST_F(MaapiCompileFixture, StaleOptionSelectionEmitsNonBlockingWarning)
{
    // Build a context whose globalOptions references "ghost-option" — not
    // declared in interface_compile.jsonc's catalog.
    const auto path = FixturePath("interface_compile.jsonc").generic_string();
    const auto context =
        R"({"adapter":{"interfacePath":")" + std::string(path)
        + "\",\"executionPolicy\":{\"failFast\":true}},\"pi\":{"
          "\"controllerName\":\"Android\","
          "\"resourceName\":\"Official\","
          "\"globalOptions\":[{\"optionName\":\"ghost-option\","
          "\"kind\":\"select\",\"selectedCases\":[\"x\"]}],"
          "\"tasks\":[{\"taskName\":\"DailyFarm\",\"enabled\":true}]}}";
    auto result = Compile(context, "execution");
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    // Non-blocking: compile still succeeds despite the stale key.
    ASSERT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false))
        << "stale option selection must not block compile";
    ASSERT_TRUE(
        (*obj)[std::string_view("canExecute")].as_bool().value_or(false));

    auto diags = (*obj)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diags.has_value());
    bool found_stale = false;
    for (auto it = diags->begin(); it != diags->end(); ++it)
    {
        auto d = it->as_object();
        if (!d)
        {
            continue;
        }
        if ((*d)[std::string_view("code")].as_string().value_or("")
                != "stale-option-selection"
            || (*d)[std::string_view("severity")].as_string().value_or("")
                   != "warning")
        {
            continue;
        }
        if ((*d)[std::string_view("message")].as_string().value_or("").find(
                "ghost-option")
            != std::string::npos)
        {
            found_stale = true;
            break;
        }
    }
    EXPECT_TRUE(found_stale)
        << "stale-option-selection warning must name the stale key";
}

// =============================================================================
// P0-3 unit tests: ResolveI18nJson recursive PI `$key` resolution.
// Direct unit tests (no fixture / no DasHost spawn) exercising the shared
// i18n helper extracted from AuthoringProjector.cpp. Input yyjson values are
// built via ParseYyjsonFromString for compound shapes and via
// yyjson::value(std::string) for scalars. The helper returns a freshly built
// value, so each assertion re-parses the output and inspects leaves.
// =============================================================================

namespace
{
    using namespace Das::Plugins::DasMaaPi;

    PiCatalog MakeSingleLangCatalog()
    {
        PiCatalog catalog;
        catalog.translations["zh_cn"]["ctrl.label"] = "\xE6\x8E\xA7\xE5\x88\xB6"
                                                      "\xE5\x99\xA8"; // 控制器
        return catalog;
    }

    // Append the 7 ResolveI18nJson tests in the same anonymous namespace.
} // namespace

TEST(I18nResolverJson, OnDollarString_Resolves)
{
    const auto          catalog = MakeSingleLangCatalog();
    const yyjson::value input(std::string("$ctrl.label"));
    const auto          out = ResolveI18nJson(input, catalog, "zh_cn");
    EXPECT_EQ(
        std::string(out.as_string().value_or("")),
        "\xE6\x8E\xA7\xE5\x88\xB6\xE5\x99\xA8"); // 控制器
}

TEST(I18nResolverJson, OnNonDollarString_PreservesValue)
{
    const auto          catalog = MakeSingleLangCatalog();
    const yyjson::value input(std::string("adb"));
    const auto          out = ResolveI18nJson(input, catalog, "zh_cn");
    EXPECT_EQ(std::string(out.as_string().value_or("")), "adb");
}

TEST(I18nResolverJson, OnNestedObject_RecursesValues)
{
    const auto catalog = MakeSingleLangCatalog();
    const auto input = Das::Utils::ParseYyjsonFromString(
        R"({"name":"$ctrl.label","type":"win32"})");
    ASSERT_TRUE(input.has_value());
    const auto out = ResolveI18nJson(*input, catalog, "zh_cn");
    const auto out_o = out.as_object();
    ASSERT_TRUE(out_o.has_value());
    EXPECT_EQ(
        std::string(
            (*out_o)[std::string_view("name")].as_string().value_or("")),
        "\xE6\x8E\xA7\xE5\x88\xB6\xE5\x99\xA8"); // 控制器
    EXPECT_EQ(
        std::string(
            (*out_o)[std::string_view("type")].as_string().value_or("")),
        "win32");
}

TEST(I18nResolverJson, OnArray_ResolvesElements)
{
    const auto catalog = MakeSingleLangCatalog();
    const auto input =
        Das::Utils::ParseYyjsonFromString(R"(["$ctrl.label","plain"])");
    ASSERT_TRUE(input.has_value());
    const auto out = ResolveI18nJson(*input, catalog, "zh_cn");
    const auto out_a = out.as_array();
    ASSERT_TRUE(out_a.has_value());
    ASSERT_EQ(out_a->size(), 2u);
    EXPECT_EQ(
        std::string((*out_a)[0].as_string().value_or("")),
        "\xE6\x8E\xA7\xE5\x88\xB6\xE5\x99\xA8"); // 控制器
    EXPECT_EQ(std::string((*out_a)[1].as_string().value_or("")), "plain");
}

TEST(I18nResolverJson, OnNonString_Clones)
{
    const auto catalog = MakeSingleLangCatalog();
    const auto input = Das::Utils::ParseYyjsonFromString(
        R"({"num":42,"flag":true,"nothing":null})");
    ASSERT_TRUE(input.has_value());
    const auto out = ResolveI18nJson(*input, catalog, "zh_cn");
    const auto out_o = out.as_object();
    ASSERT_TRUE(out_o.has_value());
    EXPECT_EQ((*out_o)[std::string_view("num")].as_int().value_or(0), 42);
    EXPECT_TRUE((*out_o)[std::string_view("flag")].as_bool().value_or(false));
    EXPECT_TRUE((*out_o)[std::string_view("nothing")].is_null());
}

TEST(I18nResolverJson, OnMissingLanguage_FallsBackToFirstAvailable)
{
    const auto          catalog = MakeSingleLangCatalog(); // only zh_cn
    const yyjson::value input(std::string("$ctrl.label"));
    // Request en_us which is absent; helper must fall back to zh_cn.
    const auto out = ResolveI18nJson(input, catalog, "en_us");
    EXPECT_EQ(
        std::string(out.as_string().value_or("")),
        "\xE6\x8E\xA7\xE5\x88\xB6\xE5\x99\xA8"); // 控制器
}

TEST(I18nResolverJson, OnEmptyCatalog_ReturnsKeyWithoutDollar)
{
    PiCatalog           empty_catalog;
    const yyjson::value input(std::string("$ctrl.label"));
    const auto          out = ResolveI18nJson(input, empty_catalog, "zh_cn");
    EXPECT_EQ(std::string(out.as_string().value_or("")), "ctrl.label");
}
