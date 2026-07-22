#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <gtest/gtest.h>

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
                    MakeDasGuid(std::string(kTaskGuidText)),
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
            auto request = ParseDasJson("{\"purpose\":\"" + purpose + "\"}");
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
        return "{\"adapter\":{\"interfacePath\":\"" + std::string(path)
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
        return "{\"adapter\":{\"interfacePath\":\"" + std::string(path)
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
