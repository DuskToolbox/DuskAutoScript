#include "../src/AgentProcessRunner.h"
#include "../src/MaapiTask.h"
#include "../src/PretaskRunner.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/PiCompiler.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::AgentRuntime;
    using namespace Das::Plugins::DasMaaPi::Test;

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

    yyjson::value EnvelopeValue(
        bool        fail_fast = true,
        std::string plugin_guid = std::string(kPluginGuidText),
        std::string task_guid = std::string(kTaskGuidText),
        bool        requires_agent = false,
        std::string tasks = R"([
          {"taskName":"DailyFarm","entry":"StartDaily","pipelineOverride":{"Stage":{"value":"one"}}},
          {"taskName":"Base","entry":"StartBase","pipelineOverride":{"Stage":{"value":"two"}}}
        ])",
        std::string controller_json =
            R"({"name":"Android","type":"Adb","readPath":"","address":"","adbPath":"adb","configJson":"{}","agentPath":""})",
        std::string agent_json = {})
    {
        auto json =
            R"({"version":1,"pluginGuid":")" + plugin_guid
            + R"(","taskTypeGuid":")" + task_guid
            + R"(","maapi":{"interfaceDirectory":"C:/maa","controllerName":"Android","controller":)"
            + controller_json
            + R"(,"resourceName":"Official",)"
              R"("resourcePaths":["C:/maa/resource","C:/maa/attach"],"resourceHash":"hash-expected",)"
              R"("failFast":)"
            + std::string(fail_fast ? "true" : "false")
            + R"(,"requiresAgentRuntime":)"
            + std::string(requires_agent ? "true" : "false") + R"(,"agent":)"
            + std::string(
                requires_agent
                    ? (agent_json.empty()
                           ? R"([{"childExec":"agent.exe","childArgs":["--serve"],"identifier":null,"timeoutMs":1000}])"
                           : agent_json)
                    : R"([])")
            + R"(,"piEnv":{"interfaceVersion":"2","clientName":"DAS","clientVersion":"0.1","clientLanguage":"cpp","clientMaafwVersion":"test","projectVersion":"","controllerJson":)"
            + JsonStringLiteral(controller_json)
            + R"(,"resourceJson":"{\"name\":\"Official\"}"},"controllerInitParams":null,"pretasks":[],"tasks":)"
            + tasks + "}}";
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
    }

    ExecutionEnvelopeDto Envelope(
        bool        fail_fast = true,
        bool        requires_agent = false,
        std::string tasks = R"([
          {"taskName":"DailyFarm","entry":"StartDaily","pipelineOverride":{"Stage":{"value":"one"}}},
          {"taskName":"Base","entry":"StartBase","pipelineOverride":{"Stage":{"value":"two"}}}
        ])",
        std::string controller_json =
            R"({"name":"Android","type":"Adb","readPath":"","address":"","adbPath":"adb","configJson":"{}","agentPath":""})")
    {
        auto parsed = ParseExecutionEnvelope(EnvelopeValue(
            fail_fast,
            std::string(kPluginGuidText),
            std::string(kTaskGuidText),
            requires_agent,
            std::move(tasks),
            std::move(controller_json)));
        EXPECT_EQ(parsed.result, DAS_S_OK);
        return std::move(parsed.envelope);
    }

    DasPtr<ExportInterface::IDasJson> ParseDasJson(std::string json)
    {
        DasPtr<ExportInterface::IDasJson> result;
        EXPECT_EQ(ParseDasJsonFromString(json.c_str(), result.Put()), DAS_S_OK);
        return result;
    }

    std::string EnvelopeJson(
        bool        fail_fast = true,
        std::string plugin_guid = std::string(kPluginGuidText),
        std::string task_guid = std::string(kTaskGuidText),
        bool        requires_agent = false,
        std::string tasks = R"([
          {"taskName":"DailyFarm","entry":"StartDaily","pipelineOverride":{"Stage":{"value":"one"}}}
        ])")
    {
        auto value = EnvelopeValue(
            fail_fast,
            std::move(plugin_guid),
            std::move(task_guid),
            requires_agent,
            std::move(tasks));
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        return serialized.value_or("{}");
    }

    class RequestedStopToken final
        : public PluginInterface::DasStopTokenImplBase<RequestedStopToken>
    {
    public:
        DasResult StopRequested(bool* canStop) override
        {
            if (!canStop)
            {
                return DAS_E_INVALID_POINTER;
            }
            *canStop = true;
            return DAS_S_OK;
        }
    };

    struct FakeProcessState
    {
        bool                   running = true;
        int                    wait_calls = 0;
        int                    terminate_calls = 0;
        std::optional<int32_t> exit_code;
    };

    class FakeProcess final : public IAgentProcess
    {
    public:
        explicit FakeProcess(std::shared_ptr<FakeProcessState> state)
            : state_(std::move(state))
        {
        }

        AgentProcessSnapshot Snapshot() const override
        {
            return AgentProcessSnapshot{
                .running = state_->running,
                .pid = 6001,
                .exit_code = state_->exit_code,
                .stdout_tail = {},
                .stderr_tail = {}};
        }

        bool WaitForExit(std::chrono::milliseconds) override
        {
            ++state_->wait_calls;
            state_->running = false;
            state_->exit_code = 0;
            return true;
        }

        void Terminate() override
        {
            ++state_->terminate_calls;
            state_->running = false;
        }

    private:
        std::shared_ptr<FakeProcessState> state_;
    };

    class FakeProcessRunner final : public IAgentProcessRunner
    {
    public:
        AgentProcessLaunchResult Launch(
            const AgentProcessLaunchRequest& request) override
        {
            launches.push_back(request);
            auto state = std::make_shared<FakeProcessState>();
            processes.push_back(state);
            return AgentProcessLaunchResult::Success(
                std::make_unique<FakeProcess>(std::move(state)));
        }

        std::vector<AgentProcessLaunchRequest>         launches;
        std::vector<std::shared_ptr<FakeProcessState>> processes;
    };

    class ScopedRuntimeHooks final
    {
    public:
        ScopedRuntimeHooks(FakeMaaApiBoundary& fake, FakeProcessRunner& runner)
        {
            SetMaaApiBoundaryForTest(&fake);
            SetAgentProcessRunnerForTest(&runner);
        }

        ~ScopedRuntimeHooks()
        {
            SetAgentProcessRunnerForTest(nullptr);
            SetMaaApiBoundaryForTest(nullptr);
        }
    };
} // namespace

TEST(DasMaaPiRuntime, RuntimeCallSequenceAndCleanup)
{
    FakeMaaApiBoundary fake;
    auto               result = MaaRuntime::Run(Envelope(), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_S_OK);
    EXPECT_EQ(
        result.completed_tasks,
        (std::vector<std::string>{"DailyFarm", "Base"}));

    EXPECT_EQ(fake.calls[0], "CreateResource");
    EXPECT_TRUE(fake.Contains("LoadResource:C:/maa/resource"));
    EXPECT_TRUE(fake.Contains("LoadResource:C:/maa/attach"));
    EXPECT_TRUE(fake.Contains("GetResourceHash"));
    EXPECT_TRUE(fake.Contains("CreateController:Android:Adb"));
    EXPECT_TRUE(fake.Contains("CreateTasker"));
    EXPECT_TRUE(fake.Contains("BindResource"));
    EXPECT_TRUE(fake.Contains("BindController"));
    EXPECT_TRUE(
        fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
    EXPECT_TRUE(
        fake.Contains("PostTask:StartBase:{\"Stage\":{\"value\":\"two\"}}"));
    EXPECT_TRUE(fake.Contains("DestroyTasker:3"));
    EXPECT_TRUE(fake.Contains("DestroyController:2"));
    EXPECT_TRUE(fake.Contains("DestroyResource:1"));
}

TEST(DasMaaPiRuntime, RuntimePassesTypedAdbCamelCaseControllerFields)
{
    FakeMaaApiBoundary fake;
    auto               result = MaaRuntime::Run(
        Envelope(
            true,
            false,
            R"([{"taskName":"DailyFarm","entry":"StartDaily","pipelineOverride":{}}])",
            R"({"name":"Android","type":"Adb","readPath":"","address":"127.0.0.1:5555","adbPath":"C:/tools/adb.exe","configJson":"{\"touch\":\"maatouch\"}","agentPath":"C:/maa/agent"})"),
        fake,
        nullptr);

    EXPECT_EQ(result.das_result, DAS_S_OK);
    ASSERT_TRUE(fake.last_controller_spec.has_value());
    EXPECT_EQ(fake.last_controller_spec->name, "Android");
    EXPECT_EQ(fake.last_controller_spec->type, "Adb");
    EXPECT_EQ(fake.last_controller_spec->address, "127.0.0.1:5555");
    EXPECT_EQ(fake.last_controller_spec->adb_path, "C:/tools/adb.exe");
    EXPECT_EQ(
        fake.last_controller_spec->config_json,
        R"({"touch":"maatouch"})");
    EXPECT_EQ(fake.last_controller_spec->agent_path, "C:/maa/agent");
}

TEST(DasMaaPiRuntime, RuntimeRejectsSnakeCaseControllerFields)
{
    // default_caster 要求 camelCase key（field_name_rule=snake_to_camel）；
    // snake_case controller 字段（adb_path/config/agent_path）缺对应 camelCase
    // key（adbPath/configJson/agentPath），聚合反射抛 →
    // DAS_E_INVALID_ARGUMENT。
    auto value = EnvelopeValue(
        true,
        std::string(kPluginGuidText),
        std::string(kTaskGuidText),
        false,
        R"([{"taskName":"T","entry":"E","pipelineOverride":{}}])",
        R"({"name":"Android","type":"Adb","readPath":"","address":"emulator-5554","adb_path":"adb-custom","config":"{}","agent_path":"x"})");
    auto parsed = ParseExecutionEnvelope(value);
    EXPECT_TRUE(DAS::IsFailed(parsed.result));
}

TEST(DasMaaPiRuntime, RuntimeRejectsConfigObjectInput)
{
    // default_caster 要求 ControllerSpec.config_json 为 string；configJson 是
    // object 时类型不匹配，聚合反射抛 → DAS_E_INVALID_ARGUMENT。
    auto value = EnvelopeValue(
        true,
        std::string(kPluginGuidText),
        std::string(kTaskGuidText),
        false,
        R"([{"taskName":"T","entry":"E","pipelineOverride":{}}])",
        R"({"name":"Android","type":"Adb","readPath":"","address":"","adbPath":"adb","configJson":{"touch":"maatouch"},"agentPath":""})");
    auto parsed = ParseExecutionEnvelope(value);
    EXPECT_TRUE(DAS::IsFailed(parsed.result));
}

TEST(DasMaaPiRuntime, RuntimePassesTypedDbgControllerReadPath)
{
    FakeMaaApiBoundary fake;
    auto               result = MaaRuntime::Run(
        Envelope(
            true,
            false,
            R"([{"taskName":"DailyFarm","entry":"StartDaily","pipelineOverride":{}}])",
            R"({"name":"Debug","type":"Dbg","readPath":"C:/maa/debug","address":"","adbPath":"adb","configJson":"{}","agentPath":""})"),
        fake,
        nullptr);

    EXPECT_EQ(result.das_result, DAS_S_OK);
    ASSERT_TRUE(fake.last_controller_spec.has_value());
    EXPECT_EQ(fake.last_controller_spec->type, "Dbg");
    EXPECT_EQ(fake.last_controller_spec->read_path, "C:/maa/debug");
}

TEST(DasMaaPiRuntime, RuntimeSourceDoesNotExtractControllerFieldsFromRawJson)
{
    const auto source_path =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "src"
        / "MaaRuntime.cpp";
    std::ifstream input(source_path);
    ASSERT_TRUE(input.is_open()) << source_path.string();

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto source = buffer.str();
    EXPECT_EQ(source.find("StringField(spec.raw_json"), std::string::npos);
    EXPECT_EQ(
        source.find("ParseYyjsonFromString(spec.raw_json"),
        std::string::npos);
}

TEST(DasMaaPiRuntime, RuntimeCleanupOnProviderFailure)
{
    FakeMaaApiBoundary fake;
    fake.post_result_by_entry["StartDaily"] =
        MaaApiResult::Failure(17, "provider post failed");

    auto result = MaaRuntime::Run(Envelope(), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_E_FAIL);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().provider_code.value_or(0), 17);
    EXPECT_TRUE(fake.Contains("DestroyTasker:3"));
    EXPECT_TRUE(fake.Contains("DestroyController:2"));
    EXPECT_TRUE(fake.Contains("DestroyResource:1"));
}

TEST(DasMaaPiRuntime, RuntimeAddsWarningOnResourceHashMismatch)
{
    FakeMaaApiBoundary fake;
    fake.resource_hash = "other-hash";

    auto result = MaaRuntime::Run(Envelope(), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_S_OK);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().severity, "warning");
    EXPECT_EQ(result.diagnostics.front().code, "resource-hash-mismatch");
}

TEST(DasMaaPiRuntime, RuntimeFailFastStopsRemainingTasksByDefault)
{
    FakeMaaApiBoundary fake;
    fake.wait_status_by_entry["StartDaily"] = MaaTaskStatus::Failed;

    auto result = MaaRuntime::Run(Envelope(true), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_E_FAIL);
    EXPECT_TRUE(
        fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
    EXPECT_FALSE(
        fake.Contains("PostTask:StartBase:{\"Stage\":{\"value\":\"two\"}}"));
}

TEST(DasMaaPiRuntime, RuntimeContinuePolicyPostsRemainingTasks)
{
    FakeMaaApiBoundary fake;
    fake.wait_status_by_entry["StartDaily"] = MaaTaskStatus::Failed;

    auto result = MaaRuntime::Run(Envelope(false), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_E_FAIL);
    EXPECT_TRUE(
        fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
    EXPECT_TRUE(
        fake.Contains("PostTask:StartBase:{\"Stage\":{\"value\":\"two\"}}"));
}

TEST(DasMaaPiRuntime, RuntimeStopTokenMapsToMaaStop)
{
    FakeMaaApiBoundary fake;
    RequestedStopToken stop;

    auto result = MaaRuntime::Run(Envelope(), fake, &stop);
    EXPECT_EQ(result.das_result, DAS_E_FAIL);
    EXPECT_TRUE(result.stopped);
    EXPECT_TRUE(fake.Contains("PostStop"));
    EXPECT_FALSE(
        fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
}

TEST(DasMaaPiRuntime, DoRejectsMissingEnvelope)
{
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, nullptr), DAS_E_INVALID_POINTER);
}

TEST(DasMaaPiRuntime, DoRejectsMalformedJsonEnvelope)
{
    auto      json = ParseDasJson("\"not-an-object\"");
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_E_INVALID_JSON);
}

TEST(DasMaaPiRuntime, DoRejectsWrongGuid)
{
    auto      json = ParseDasJson(EnvelopeJson(
        true,
        "00000000-0000-0000-0000-000000000000",
        std::string(kTaskGuidText)));
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_E_INVALID_ARGUMENT);
}

TEST(DasMaaPiRuntime, DoRejectsMissingTasks)
{
    auto      json = ParseDasJson(EnvelopeJson(
        true,
        std::string(kPluginGuidText),
        std::string(kTaskGuidText),
        false,
        "[]"));
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_E_INVALID_ARGUMENT);
}

TEST(DasMaaPiRuntime, DoRunsAgentRequiredEnvelopeThroughSharedRuntime)
{
    auto               json = ParseDasJson(EnvelopeJson(
        true,
        std::string(kPluginGuidText),
        std::string(kTaskGuidText),
        true));
    FakeMaaApiBoundary fake;
    FakeProcessRunner  runner;
    ScopedRuntimeHooks hooks(fake, runner);

    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_S_OK);
    ASSERT_EQ(runner.launches.size(), 1u);
    EXPECT_EQ(
        runner.launches[0].arguments,
        (std::vector<std::string>{"--serve", "agent-client-id"}));
    EXPECT_TRUE(fake.Contains("ConnectAgentClient"));
    EXPECT_TRUE(fake.Contains("RegisterAgentClientTaskerSink"));
    EXPECT_TRUE(
        fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
    ASSERT_EQ(runner.processes.size(), 1u);
    EXPECT_GT(runner.processes[0]->wait_calls, 0);
}

TEST(DasMaaPiRuntime, ParseAcceptsAgentObjectAsSingleElement)
{
    // PI V2 `agent: object | object[]`：单对象形态经 custom caster 解析成 1
    // 元素 vector。
    auto value = EnvelopeValue(
        true,
        std::string(kPluginGuidText),
        std::string(kTaskGuidText),
        true,
        R"([{"taskName":"DailyFarm","entry":"StartDaily","pipelineOverride":{}}])",
        R"({"name":"Android","type":"Adb","readPath":"","address":"","adbPath":"adb","configJson":"{}","agentPath":""})",
        R"({"childExec":"agent.exe","childArgs":["--serve"],"identifier":null,"timeoutMs":1000})");
    auto parsed = ParseExecutionEnvelope(value);
    ASSERT_EQ(parsed.result, DAS_S_OK);
    ASSERT_EQ(parsed.envelope.maapi.agent.size(), 1u);
    EXPECT_EQ(parsed.envelope.maapi.agent[0].child_exec, "agent.exe");
    EXPECT_EQ(
        parsed.envelope.maapi.agent[0].child_args,
        (std::vector<std::string>{"--serve"}));
    EXPECT_EQ(parsed.envelope.maapi.agent[0].timeout_ms, 1000);
    EXPECT_FALSE(parsed.envelope.maapi.agent[0].identifier.has_value());
}

TEST(DasMaaPiRuntime, ParseAcceptsAgentArrayWithMultipleElements)
{
    // 数组形态解析成多元素 vector，并覆盖 optional identifier 字段。
    auto value = EnvelopeValue(
        true,
        std::string(kPluginGuidText),
        std::string(kTaskGuidText),
        true,
        R"([{"taskName":"DailyFarm","entry":"StartDaily","pipelineOverride":{}}])",
        R"({"name":"Android","type":"Adb","readPath":"","address":"","adbPath":"adb","configJson":"{}","agentPath":""})",
        R"([{"childExec":"a.exe","childArgs":["--x"],"identifier":null,"timeoutMs":500},)"
        R"({"childExec":"b.exe","childArgs":["--y"],"identifier":"id1","timeoutMs":700}])");
    auto parsed = ParseExecutionEnvelope(value);
    ASSERT_EQ(parsed.result, DAS_S_OK);
    ASSERT_EQ(parsed.envelope.maapi.agent.size(), 2u);
    EXPECT_EQ(parsed.envelope.maapi.agent[0].child_exec, "a.exe");
    EXPECT_EQ(parsed.envelope.maapi.agent[1].child_exec, "b.exe");
    ASSERT_TRUE(parsed.envelope.maapi.agent[1].identifier.has_value());
    EXPECT_EQ(*parsed.envelope.maapi.agent[1].identifier, "id1");
    EXPECT_EQ(parsed.envelope.maapi.agent[1].timeout_ms, 700);
}

TEST(DasMaaPiRuntime, ParseReadsControllerFromExplicitObject)
{
    // controller 作为非 optional 字段，default_caster 直接读显式 controller
    // 对象 （PiCompiler 总产出 controller 对象，运行时无 fallback 路径）。
    auto value = EnvelopeValue(
        true,
        std::string(kPluginGuidText),
        std::string(kTaskGuidText),
        false,
        R"([{"taskName":"T","entry":"E","pipelineOverride":{}}])",
        R"({"name":"Explicit","type":"Adb","readPath":"","address":"","adbPath":"C:/explicit/adb.exe","configJson":"{}","agentPath":"explicit/agent"})");
    auto parsed = ParseExecutionEnvelope(value);
    ASSERT_EQ(parsed.result, DAS_S_OK);
    EXPECT_EQ(parsed.envelope.maapi.controller.name, "Explicit");
    EXPECT_EQ(parsed.envelope.maapi.controller.adb_path, "C:/explicit/adb.exe");
    EXPECT_EQ(parsed.envelope.maapi.controller.agent_path, "explicit/agent");
}

TEST(DasMaaPiRuntime, DoRunsValidNonAgentEnvelope)
{
    FakeMaaApiBoundary fake;
    SetMaaApiBoundaryForTest(&fake);

    auto      json = ParseDasJson(EnvelopeJson());
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_S_OK);
    EXPECT_TRUE(
        fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));

    SetMaaApiBoundaryForTest(nullptr);
}

// Feature 4 RED — typed controller dispatch + Tier2 platform stub.
// MaaRuntime::Run must route to CreateControllerFromParams when the envelope
// carries win32/gamepad init params, and reject Tier2 types with a structured
// controller-type-unsupported-on-platform diagnostic.

namespace
{
    ExecutionEnvelopeDto EnvelopeWithTypedController(
        ControllerSpec                   spec,
        ControllerInitParams             init_params,
        std::vector<MaaTaskExecutionDto> tasks = {})
    {
        ExecutionEnvelopeDto envelope;
        envelope.maapi.interface_directory = "C:/maa";
        envelope.maapi.fail_fast = true;
        envelope.maapi.controller_name = spec.name;
        envelope.maapi.controller = std::move(spec);
        envelope.maapi.controller_init_params = std::move(init_params);
        envelope.maapi.resource_name = "Official";
        envelope.maapi.resource_paths.emplace_back("C:/maa/resource");
        if (tasks.empty())
        {
            MaaTaskExecutionDto task;
            task.task_name = "DailyFarm";
            task.entry = "StartDaily";
            envelope.maapi.tasks.emplace_back(std::move(task));
        }
        else
        {
            envelope.maapi.tasks = std::move(tasks);
        }
        return envelope;
    }
} // namespace

TEST(
    DasMaaPiRuntime,
    TypedWin32ControllerDispatchesToCreateControllerFromParams)
{
    FakeMaaApiBoundary fake;
    ControllerSpec     spec;
    spec.name = "Desktop";
    spec.type = "Win32";
    ControllerInitParams params;
    params.controller_type = "Win32";
    Win32InitParams win32;
    win32.class_regex = "App.*";
    win32.window_regex = ".*";
    win32.screencap_method = 1ULL;
    win32.mouse_method = 1ULL;
    win32.keyboard_method = 1ULL << 1;
    params.win32 = win32;

    auto result = MaaRuntime::Run(
        EnvelopeWithTypedController(spec, std::move(params)),
        fake,
        nullptr);
    EXPECT_EQ(result.das_result, DAS_S_OK);
    EXPECT_TRUE(fake.Contains("CreateControllerFromParams:Desktop:Win32"));
    ASSERT_TRUE(fake.last_controller_init_params.has_value());
    EXPECT_EQ(fake.last_controller_init_params->controller_type, "Win32");
    ASSERT_TRUE(fake.last_controller_init_params->win32.has_value());
    EXPECT_EQ(fake.last_controller_init_params->win32->class_regex, "App.*");
    // Legacy single-arg path must NOT have been invoked for typed controllers.
    EXPECT_FALSE(fake.Contains("CreateController:Desktop:Win32"));
}

TEST(
    DasMaaPiRuntime,
    TypedGamepadControllerDispatchesToCreateControllerFromParams)
{
    FakeMaaApiBoundary fake;
    ControllerSpec     spec;
    spec.name = "Gamepad1";
    spec.type = "Gamepad";
    ControllerInitParams params;
    params.controller_type = "Gamepad";
    GamepadInitParams gamepad;
    gamepad.class_regex = "Xbox.*";
    gamepad.window_regex = ".*";
    gamepad.gamepad_type = 1ULL;
    gamepad.screencap_method = 1ULL;
    params.gamepad = gamepad;

    auto result = MaaRuntime::Run(
        EnvelopeWithTypedController(spec, std::move(params)),
        fake,
        nullptr);
    EXPECT_EQ(result.das_result, DAS_S_OK);
    EXPECT_TRUE(fake.Contains("CreateControllerFromParams:Gamepad1:Gamepad"));
    ASSERT_TRUE(fake.last_controller_init_params.has_value());
    ASSERT_TRUE(fake.last_controller_init_params->gamepad.has_value());
    EXPECT_EQ(fake.last_controller_init_params->gamepad->gamepad_type, 1ULL);
}

TEST(DasMaaPiRuntime, TypedControllerBoundaryFailureSurfacesAsDiagnostic)
{
    FakeMaaApiBoundary fake;
    fake.create_from_params_result.handle = kInvalidMaaControllerHandle;
    fake.create_from_params_result.error_code = "controller-window-not-found";
    fake.create_from_params_result.error_message =
        "No desktop window matched class_regex='Missing.*'";

    ControllerSpec spec;
    spec.name = "Desktop";
    spec.type = "Win32";
    ControllerInitParams params;
    params.controller_type = "Win32";
    params.win32 = Win32InitParams{};

    auto result = MaaRuntime::Run(
        EnvelopeWithTypedController(spec, std::move(params)),
        fake,
        nullptr);
    EXPECT_NE(result.das_result, DAS_S_OK);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.back().code, "controller-window-not-found");
    EXPECT_NE(
        result.diagnostics.back().message.find("class_regex='Missing.*'"),
        std::string::npos);
}

TEST(DasMaaPiRuntime, Tier2ControllerTypesRejectedOnWindows)
{
    // MacOS/PlayCover/WlRoots controllers are not linkable on Windows — the
    // production ScopedMaaApiBoundary Tier2 stub returns kInvalid with a
    // structured controller-type-unsupported-on-platform code. MaaRuntime
    // surfaces it as a diagnostic without calling the single-arg overload.
    for (const auto* type : {"MacOS", "PlayCover", "WlRoots"})
    {
        FakeMaaApiBoundary fake;
        fake.create_from_params_result.handle = kInvalidMaaControllerHandle;
        fake.create_from_params_result.error_code =
            "controller-type-unsupported-on-platform";
        fake.create_from_params_result.error_message =
            std::string("Controller type ") + type
            + " is not available on this platform";

        ControllerSpec spec;
        spec.name = type;
        spec.type = type;
        ControllerInitParams params;
        params.controller_type = type;

        auto result = MaaRuntime::Run(
            EnvelopeWithTypedController(spec, std::move(params)),
            fake,
            nullptr);
        EXPECT_NE(result.das_result, DAS_S_OK);
        ASSERT_FALSE(result.diagnostics.empty());
        EXPECT_EQ(
            result.diagnostics.back().code,
            "controller-type-unsupported-on-platform");
    }
}

TEST(DasMaaPiRuntime, LegacyAdbPathUnaffectedByTypedDispatch)
{
    // Adb envelope without controller_init_params must keep using the legacy
    // single-arg CreateController overload — no regression in the existing
    // happy path.
    FakeMaaApiBoundary fake;
    auto               result = MaaRuntime::Run(Envelope(), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_S_OK);
    EXPECT_TRUE(fake.Contains("CreateController:Android:Adb"));
    EXPECT_FALSE(fake.last_controller_init_params.has_value());
}

// ============================================================================
// PI v2.7.0 pretask execution stage (D-86-02).
// pretask runs AFTER LoadResource and BEFORE CreateController. Spawn failure
// or nonzero exit aborts the run with DAS_E_MAAPI_PRETASK_FAILED + structured
// diagnostic (pretask-failed / pretask-nonzero-exit). CWD is the PI interface
// directory.
// ============================================================================

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

    ExecutionEnvelopeDto EnvelopeWithPretasks(
        std::vector<MaaPretaskDto> pretasks,
        std::string                interface_directory = "C:/maa")
    {
        ExecutionEnvelopeDto envelope;
        envelope.maapi.interface_directory = std::move(interface_directory);
        envelope.maapi.fail_fast = true;
        envelope.maapi.controller_name = "Android";
        envelope.maapi.controller.type = "Adb";
        envelope.maapi.resource_name = "Official";
        envelope.maapi.resource_paths.emplace_back("C:/maa/resource");
        envelope.maapi.pretasks = std::move(pretasks);
        MaaTaskExecutionDto task;
        task.task_name = "DailyFarm";
        task.entry = "StartDaily";
        envelope.maapi.tasks.emplace_back(std::move(task));
        return envelope;
    }

    MaaPretaskDto MakePretask(
        std::string              exec,
        std::vector<std::string> args = {})
    {
        MaaPretaskDto pretask;
        pretask.name = "pre";
        pretask.exec = std::move(exec);
        pretask.args = std::move(args);
        return pretask;
    }

    class ScopedPretaskRunner final
    {
    public:
        explicit ScopedPretaskRunner(IPretaskRunner* runner)
        {
            SetPretaskRunnerForTest(runner);
        }
        ~ScopedPretaskRunner() { SetPretaskRunnerForTest(nullptr); }
    };
} // namespace

TEST(DasMaaPiRuntime, EmptyPretasksSkipsRunnerAndReachesController)
{
    FakeMaaApiBoundary  fake;
    FakePretaskRunner   runner;
    ScopedPretaskRunner hook(&runner);
    auto result = MaaRuntime::Run(EnvelopeWithPretasks({}), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_S_OK);
    EXPECT_TRUE(runner.launches.empty());
    EXPECT_TRUE(fake.Contains("CreateController:Android:Adb"));
}

TEST(DasMaaPiRuntime, PretaskZeroExitProceedsToControllerCreate)
{
    FakeMaaApiBoundary  fake;
    FakePretaskRunner   runner;
    ScopedPretaskRunner hook(&runner);
    auto                result = MaaRuntime::Run(
        EnvelopeWithPretasks({MakePretask("tools/pre.exe", {"--init"})}),
        fake,
        nullptr);
    EXPECT_EQ(result.das_result, DAS_S_OK);
    ASSERT_EQ(runner.launches.size(), 1u);
    EXPECT_EQ(runner.launches[0].exec, "tools/pre.exe");
    ASSERT_EQ(runner.launches[0].args.size(), 1u);
    EXPECT_EQ(runner.launches[0].args[0], "--init");
    EXPECT_EQ(
        runner.launches[0].working_directory.string(),
        std::filesystem::path("C:/maa").string());
    // Ordering: pretask ran before controller create. Controller still created.
    EXPECT_TRUE(fake.Contains("CreateController:Android:Adb"));
}

TEST(DasMaaPiRuntime, PretaskNonzeroExitAbortsBeforeControllerCreate)
{
    FakeMaaApiBoundary fake;
    FakePretaskRunner  runner;
    PretaskRunResult   nonzero;
    nonzero.launched = true;
    nonzero.exit_code = 7;
    runner.scripted_results.push_back(nonzero);
    ScopedPretaskRunner hook(&runner);

    auto result = MaaRuntime::Run(
        EnvelopeWithPretasks({MakePretask("tools/pre.exe")}),
        fake,
        nullptr);
    EXPECT_EQ(result.das_result, DAS_E_MAAPI_PRETASK_FAILED);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.back().code, "pretask-nonzero-exit");
    ASSERT_TRUE(result.diagnostics.back().provider_code.has_value());
    EXPECT_EQ(*result.diagnostics.back().provider_code, 7);
    // Resource creation happens before the pretask stage; controller create
    // is what must be skipped when a pretask aborts the run.
    EXPECT_TRUE(fake.Contains("CreateResource"));
    EXPECT_FALSE(fake.Contains("CreateController:Android:Adb"));
}

TEST(DasMaaPiRuntime, PretaskSpawnFailureAbortsBeforeControllerCreate)
{
    FakeMaaApiBoundary fake;
    FakePretaskRunner  runner;
    PretaskRunResult   spawn_fail;
    spawn_fail.launched = false;
    spawn_fail.failure_message = "ENOENT: tools/pre.exe";
    runner.scripted_results.push_back(spawn_fail);
    ScopedPretaskRunner hook(&runner);

    auto result = MaaRuntime::Run(
        EnvelopeWithPretasks({MakePretask("tools/missing.exe")}),
        fake,
        nullptr);
    EXPECT_EQ(result.das_result, DAS_E_MAAPI_PRETASK_FAILED);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.back().code, "pretask-failed");
    EXPECT_NE(
        result.diagnostics.back().message.find("ENOENT"),
        std::string::npos);
    EXPECT_FALSE(fake.Contains("CreateController:Android:Adb"));
    EXPECT_TRUE(fake.Contains("CreateResource"));
}

// ============================================================================
// PI v2.7.0 pretask option serialization (PiCompiler side).
// Verify envelope.pretasks carries exec + args + trailing single-line compact
// JSON of selected option values, including nested sub-options activated by
// the chosen case (protocol L290, MXU pretasks.ts:169-183).
// ============================================================================

namespace
{
    PiOption MakeSelectOption(
        std::string              name,
        std::vector<std::string> case_names,
        std::vector<std::string> default_cases = {},
        std::vector<PiCaseDto>   cases_with_nested = {})
    {
        PiOption option;
        option.dto.name = std::move(name);
        option.dto.type = "select";
        if (cases_with_nested.empty())
        {
            for (auto& cn : case_names)
            {
                PiCaseDto c;
                c.name = std::move(cn);
                option.dto.cases.emplace_back(std::move(c));
            }
        }
        else
        {
            option.dto.cases = std::move(cases_with_nested);
        }
        option.dto.default_cases = std::move(default_cases);
        option.raw_pipeline_override_json = "{}";
        option.raw.raw_json = "{}";
        return option;
    }

    PiController MakeMinimalController(
        std::string name = "Android",
        std::string type = "Adb")
    {
        PiController controller;
        controller.dto.name = std::move(name);
        controller.dto.type = std::move(type);
        controller.raw.raw_json = R"({"name":")" + controller.dto.name
                                  + R"(","type":")" + controller.dto.type
                                  + R"("})";
        return controller;
    }

    PiResource MakeMinimalResource(std::string name = "Official")
    {
        PiResource resource;
        resource.dto.name = std::move(name);
        resource.dto.path.emplace_back("resource/base");
        resource.resolved_paths.emplace_back("C:/maa/resource/base");
        resource.raw.raw_json = R"({"name":")" + resource.dto.name
                                + R"(","path":["resource/base"]})";
        return resource;
    }

    PiTask MakeMinimalTask(std::string name = "T", std::string entry = "E")
    {
        PiTask task;
        task.dto.name = std::move(name);
        task.dto.entry = std::move(entry);
        task.dto.default_check = true;
        task.raw_pipeline_override_json = "{}";
        task.raw.raw_json = "{}";
        return task;
    }

    AcceptedSettingsDto MakeMinimalSettings(
        std::string controller_name = "Android",
        std::string resource_name = "Official")
    {
        AcceptedSettingsDto settings;
        settings.pi.controller_name = std::move(controller_name);
        settings.pi.resource_name = std::move(resource_name);
        MaapiPiTaskSettingsDto task;
        task.task_name = "T";
        task.enabled = true;
        settings.pi.tasks.emplace_back(std::move(task));
        return settings;
    }

    MaapiPiOptionSettingsDto MakeSelected(
        std::string              option_name,
        std::string              kind,
        std::vector<std::string> selected_cases = {})
    {
        MaapiPiOptionSettingsDto opt;
        opt.option_name = std::move(option_name);
        opt.kind = std::move(kind);
        opt.selected_cases = std::move(selected_cases);
        return opt;
    }
} // namespace

TEST(DasMaaPiPretaskCompile, FlatSelectOptionSerializedAsCompactJsonTrailingArg)
{
    PiCatalog catalog;
    catalog.controllers.emplace_back(MakeMinimalController());
    catalog.resources.emplace_back(MakeMinimalResource());
    catalog.tasks.emplace_back(MakeMinimalTask());
    catalog.options.emplace_back(MakeSelectOption("mode", {"a", "b"}, {"a"}));
    PiPretask pretask;
    pretask.dto.name = "Bootstrap";
    pretask.dto.exec = "tools/pre.exe";
    pretask.dto.args = {"--init"};
    pretask.dto.option = {"mode"};
    pretask.raw.raw_json = "{}";
    catalog.pretasks.emplace_back(std::move(pretask));

    auto settings = MakeMinimalSettings();
    settings.pi.global_options.emplace_back(
        MakeSelected("mode", "select", {"a"}));

    auto result = CompileMaapi(settings, catalog, "execute");
    ASSERT_TRUE(result.ok) << "diagnostics: "
                           << (result.diagnostics.empty()
                                   ? std::string{"(none)"}
                                   : result.diagnostics[0].message);
    ASSERT_TRUE(result.execution_input.has_value());
    ASSERT_EQ(result.execution_input->maapi.pretasks.size(), 1u);
    EXPECT_EQ(result.execution_input->maapi.pretasks[0].exec, "tools/pre.exe");
    ASSERT_EQ(result.execution_input->maapi.pretasks[0].args.size(), 2u);
    EXPECT_EQ(result.execution_input->maapi.pretasks[0].args[0], "--init");
    // Compact single-line JSON, no whitespace.
    EXPECT_EQ(
        result.execution_input->maapi.pretasks[0].args[1],
        R"({"mode":"a"})");
}

TEST(DasMaaPiPretaskCompile, NestedOptionRecursesIntoActivatedCase)
{
    // Protocol L290: when a select/switch case is activated, its nested
    // option[] entries must be added to the result object under their own
    // keys. MXU pretasks.ts:169-183.
    PiCatalog catalog;
    catalog.controllers.emplace_back(MakeMinimalController());
    catalog.resources.emplace_back(MakeMinimalResource());
    catalog.tasks.emplace_back(MakeMinimalTask());

    // "inner" is a nested select activated when "outer" = "on".
    catalog.options.emplace_back(MakeSelectOption("inner", {"x", "y"}, {"x"}));
    // "outer" select; its "on" case activates nested option "inner".
    PiCaseDto on_case;
    on_case.name = "on";
    on_case.option = {"inner"};
    PiCaseDto off_case;
    off_case.name = "off";
    catalog.options.emplace_back(
        MakeSelectOption("outer", {}, {"on"}, {on_case, off_case}));

    PiPretask pretask;
    pretask.dto.name = "Pre";
    pretask.dto.exec = "pre.exe";
    pretask.dto.option = {"outer"};
    pretask.raw.raw_json = "{}";
    catalog.pretasks.emplace_back(std::move(pretask));

    auto settings = MakeMinimalSettings();
    settings.pi.global_options.emplace_back(
        MakeSelected("outer", "select", {"on"}));
    settings.pi.global_options.emplace_back(
        MakeSelected("inner", "select", {"x"}));

    auto result = CompileMaapi(settings, catalog, "execute");
    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.execution_input.has_value());
    ASSERT_EQ(result.execution_input->maapi.pretasks.size(), 1u);
    ASSERT_EQ(result.execution_input->maapi.pretasks[0].args.size(), 1u);
    // Both outer and nested inner must appear in the compact JSON, with outer's
    // selected case and inner's selected case. Order: outer first (encountered
    // first), then inner (added by recursion).
    EXPECT_EQ(
        result.execution_input->maapi.pretasks[0].args[0],
        R"({"outer":"on","inner":"x"})");
}

TEST(DasMaaPiPretaskCompile, PretaskWithNoOptionsOmitsTrailingJsonArg)
{
    PiCatalog catalog;
    catalog.controllers.emplace_back(MakeMinimalController());
    catalog.resources.emplace_back(MakeMinimalResource());
    catalog.tasks.emplace_back(MakeMinimalTask());

    PiPretask pretask;
    pretask.dto.name = "NoOpt";
    pretask.dto.exec = "pre.exe";
    pretask.dto.args = {"--flag"};
    pretask.raw.raw_json = "{}";
    catalog.pretasks.emplace_back(std::move(pretask));

    auto settings = MakeMinimalSettings();
    auto result = CompileMaapi(settings, catalog, "execute");
    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.execution_input.has_value());
    ASSERT_EQ(result.execution_input->maapi.pretasks.size(), 1u);
    // No option declared → no trailing JSON arg appended.
    ASSERT_EQ(result.execution_input->maapi.pretasks[0].args.size(), 1u);
    EXPECT_EQ(result.execution_input->maapi.pretasks[0].args[0], "--flag");
}
