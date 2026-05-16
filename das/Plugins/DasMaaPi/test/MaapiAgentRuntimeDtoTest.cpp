#include "../src/AgentRuntimeRequest.h"

#include <das/Plugins/DasMaaPi/AgentRuntimeDto.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace Das::Plugins::DasMaaPi::AgentRuntime;

    std::string DiagnosticText(
        const std::vector<AgentDiagnosticDto>& diagnostics)
    {
        std::ostringstream output;
        for (const auto& diagnostic : diagnostics)
        {
            output << diagnostic.code << ": " << diagnostic.message << '\n';
        }
        return output.str();
    }

    const PiEnvVarDto* FindEnv(
        const std::vector<PiEnvVarDto>& env,
        std::string_view                key)
    {
        auto it = std::find_if(
            env.begin(),
            env.end(),
            [key](const PiEnvVarDto& item) { return item.key == key; });
        return it == env.end() ? nullptr : &*it;
    }

    yyjson::value ParseJson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
    }
} // namespace

TEST(DasMaaPiAgentRuntimeDto, ParsesStartRequestFromLowerCamelJson)
{
    auto parsed = NormalizeAgentRuntimeDispatch(
        "start",
        R"({
          "version": 1,
          "operation": "start",
          "runtimeRef": {
            "kind": "maapiRuntimeSession",
            "sessionId": "runtime-1"
          },
          "interfaceDirectory": "E:/maa/project",
          "agents": [
            {
              "childExec": "python",
              "childArgs": ["./agent/main.py"],
              "identifier": null,
              "timeoutMs": 9000
            }
          ],
          "piEnv": {
            "PI_INTERFACE_VERSION": "v2.6.0",
            "PI_CLIENT_NAME": "DAS",
            "PI_CLIENT_VERSION": "0.1",
            "PI_CLIENT_LANGUAGE": "zh_cn",
            "PI_CLIENT_MAAFW_VERSION": "v5.10.4",
            "PI_VERSION": "project-version",
            "PI_CONTROLLER": "{\"type\":\"Adb\"}",
            "PI_RESOURCE": "{\"name\":\"Official\"}"
          },
          "options": {
            "tcpCompatMode": true,
            "captureOutput": false,
            "stopTimeoutMs": 7000,
            "maxOutputTailBytes": 2048
          }
        })");

    ASSERT_TRUE(parsed.ok) << DiagnosticText(parsed.diagnostics);
    const auto& request = parsed.request;
    EXPECT_EQ(request.version, 1);
    EXPECT_EQ(request.operation, "start");
    ASSERT_TRUE(request.runtime_ref.has_value());
    EXPECT_EQ(request.runtime_ref->kind, "maapiRuntimeSession");
    EXPECT_EQ(request.runtime_ref->session_id, "runtime-1");
    EXPECT_EQ(request.interface_directory, "E:/maa/project");
    ASSERT_EQ(request.agents.size(), 1u);
    EXPECT_EQ(request.agents[0].child_exec, "python");
    EXPECT_EQ(request.agents[0].child_args, (std::vector<std::string>{"./agent/main.py"}));
    EXPECT_FALSE(request.agents[0].identifier.has_value());
    EXPECT_EQ(request.agents[0].timeout_ms, 9000);
    EXPECT_EQ(request.pi_env.interface_version, "v2.6.0");
    EXPECT_EQ(request.pi_env.client_name, "DAS");
    EXPECT_EQ(request.pi_env.client_version, "0.1");
    EXPECT_EQ(request.pi_env.client_language, "zh_cn");
    EXPECT_EQ(request.pi_env.client_maafw_version, "v5.10.4");
    EXPECT_EQ(request.pi_env.project_version, "project-version");
    EXPECT_EQ(request.pi_env.controller_json, R"({"type":"Adb"})");
    EXPECT_EQ(request.pi_env.resource_json, R"({"name":"Official"})");
    EXPECT_TRUE(request.options.tcp_compat_mode);
    EXPECT_FALSE(request.options.capture_output);
    EXPECT_EQ(request.options.stop_timeout_ms, 7000);
    EXPECT_EQ(request.options.max_output_tail_bytes, 2048);
}

TEST(DasMaaPiAgentRuntimeDto, SerializesStructuredResultEnvelopeLowerCamel)
{
    AgentRuntimeResultDto result;
    result.status = "succeeded";
    result.session_id = "session-1";
    result.agents.push_back(AgentStateDto{
        .agent_id = "agent-0",
        .state = "running",
        .identifier = "ipc-id",
        .pid = 1234,
        .exit_code = std::nullopt,
        .stdout_tail = "out",
        .stderr_tail = "err"});
    result.outputs.agent_session_id = "session-1";
    result.outputs.running_agent_count = 1;
    result.signals.succeeded = true;

    auto json = SerializeAgentRuntimeResultJson(result);
    auto value = ParseJson(json);
    auto obj = value.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ((*obj)[std::string_view("sessionId")].as_string().value_or(""), "session-1");
    EXPECT_FALSE(obj->contains(std::string_view("session_id")));

    auto agents = (*obj)[std::string_view("agents")].as_array();
    ASSERT_TRUE(agents.has_value());
    ASSERT_EQ(agents->size(), 1u);
    auto agent = agents->begin()->as_object();
    ASSERT_TRUE(agent.has_value());
    EXPECT_EQ((*agent)[std::string_view("agentId")].as_string().value_or(""), "agent-0");
    EXPECT_EQ((*agent)[std::string_view("stdoutTail")].as_string().value_or(""), "out");

    auto outputs = (*obj)[std::string_view("outputs")].as_object();
    ASSERT_TRUE(outputs.has_value());
    EXPECT_EQ((*outputs)[std::string_view("agentSessionId")].as_string().value_or(""), "session-1");
    EXPECT_EQ((*outputs)[std::string_view("runningAgentCount")].as_sint().value_or(0), 1);
}

TEST(DasMaaPiAgentRuntimeDto, RejectsInvalidDispatchCommandWithDiagnostics)
{
    auto parsed = NormalizeAgentRuntimeDispatch(
        "restart",
        R"({"version":1,"operation":"restart"})");

    EXPECT_FALSE(parsed.ok);
    ASSERT_FALSE(parsed.diagnostics.empty());
    EXPECT_EQ(parsed.diagnostics.front().severity, "error");
    EXPECT_EQ(parsed.diagnostics.front().code, "invalid-command");
}

TEST(DasMaaPiAgentRuntimeDto, StopAndStatusRequireSessionId)
{
    auto stop = NormalizeAgentRuntimeDispatch(
        "stop",
        R"({"version":1,"operation":"stop","sessionId":"session-1"})");
    ASSERT_TRUE(stop.ok) << DiagnosticText(stop.diagnostics);
    EXPECT_EQ(stop.request.session_id, "session-1");

    auto status = NormalizeAgentRuntimeDispatch(
        "status",
        R"({"version":1,"operation":"status","sessionId":"session-1","agentIds":["agent-0"]})");
    ASSERT_TRUE(status.ok) << DiagnosticText(status.diagnostics);
    EXPECT_EQ(status.request.session_id, "session-1");
    EXPECT_EQ(status.request.agent_ids, (std::vector<std::string>{"agent-0"}));

    auto missing = NormalizeAgentRuntimeDispatch(
        "status",
        R"({"version":1,"operation":"status"})");
    EXPECT_FALSE(missing.ok);
    ASSERT_FALSE(missing.diagnostics.empty());
    EXPECT_EQ(missing.diagnostics.front().code, "missing-session-id");
}

TEST(DasMaaPiAgentRuntimeDto, NormalizesSingleAgentObjectToAgentArray)
{
    auto parsed = NormalizeAgentRuntimeDispatch(
        "validate",
        R"({
          "version": 1,
          "operation": "validate",
          "interfaceDirectory": "E:/maa/project",
          "agent": {
            "childExec": "./agent.exe",
            "childArgs": ["--verbose"],
            "identifier": "provided-id"
          }
        })");

    ASSERT_TRUE(parsed.ok) << DiagnosticText(parsed.diagnostics);
    ASSERT_EQ(parsed.request.agents.size(), 1u);
    EXPECT_EQ(parsed.request.agents[0].child_exec, "./agent.exe");
    EXPECT_EQ(parsed.request.agents[0].child_args, (std::vector<std::string>{"--verbose"}));
    ASSERT_TRUE(parsed.request.agents[0].identifier.has_value());
    EXPECT_EQ(*parsed.request.agents[0].identifier, "provided-id");
}

TEST(DasMaaPiAgentRuntimeDto, MergesSettingsDefaultsWithInputRequest)
{
    auto parsed = MergeAgentRuntimeSettingsAndInput(
        R"({
          "options": {
            "tcpCompatMode": false,
            "captureOutput": false,
            "stopTimeoutMs": 9000,
            "maxOutputTailBytes": 4096
          }
        })",
        R"({
          "version": 1,
          "operation": "start",
          "interfaceDirectory": "E:/maa/project",
          "agent": {"childExec": "python"},
          "options": {
            "tcpCompatMode": true,
            "maxOutputTailBytes": 512
          }
        })");

    ASSERT_TRUE(parsed.ok) << DiagnosticText(parsed.diagnostics);
    EXPECT_EQ(parsed.request.operation, "start");
    EXPECT_TRUE(parsed.request.options.tcp_compat_mode);
    EXPECT_FALSE(parsed.request.options.capture_output);
    EXPECT_EQ(parsed.request.options.stop_timeout_ms, 9000);
    EXPECT_EQ(parsed.request.options.max_output_tail_bytes, 512);
}

TEST(DasMaaPiAgentRuntimeDto, FiltersLaunchEnvironmentToKnownAndExtraPiKeys)
{
    auto parsed = NormalizeAgentRuntimeDispatch(
        "start",
        R"({
          "version": 1,
          "operation": "start",
          "interfaceDirectory": "E:/maa/project",
          "agent": {"childExec": "python"},
          "piEnv": {
            "PI_CLIENT_NAME": "DAS",
            "PI_CLIENT_VERSION": "0.1",
            "PI_CUSTOM_TRACE": "1",
            "PATH": "must-not-launch"
          }
        })");

    ASSERT_TRUE(parsed.ok) << DiagnosticText(parsed.diagnostics);
    ASSERT_EQ(parsed.request.extra_pi_env.size(), 1u);
    EXPECT_EQ(parsed.request.extra_pi_env[0].key, "PI_CUSTOM_TRACE");
    EXPECT_EQ(parsed.request.extra_pi_env[0].value, "1");

    auto env = BuildLaunchEnvironment(parsed.request);
    ASSERT_NE(FindEnv(env, "PI_CLIENT_NAME"), nullptr);
    ASSERT_NE(FindEnv(env, "PI_CLIENT_VERSION"), nullptr);
    ASSERT_NE(FindEnv(env, "PI_CUSTOM_TRACE"), nullptr);
    EXPECT_EQ(FindEnv(env, "PATH"), nullptr);
    EXPECT_TRUE(std::any_of(
        parsed.diagnostics.begin(),
        parsed.diagnostics.end(),
        [](const AgentDiagnosticDto& diagnostic) {
            return diagnostic.code == "ignored-non-pi-env";
        }));
}

TEST(DasMaaPiAgentRuntimeDto, BoundsUnknownPiEnvironmentExtensions)
{
    std::string pi_env = R"({"PI_CLIENT_NAME":"DAS")";
    for (std::size_t i = 0; i <= kMaxExtraPiEnv; ++i)
    {
        pi_env += R"(,"PI_EXTRA_)" + std::to_string(i) + R"(":"value")";
    }
    pi_env += "}";

    auto parsed = NormalizeAgentRuntimeDispatch(
        "start",
        R"({"version":1,"operation":"start","interfaceDirectory":"E:/maa","agent":{"childExec":"python"},"piEnv":)"
            + pi_env + "}");

    EXPECT_FALSE(parsed.ok);
    ASSERT_FALSE(parsed.diagnostics.empty());
    EXPECT_TRUE(std::any_of(
        parsed.diagnostics.begin(),
        parsed.diagnostics.end(),
        [](const AgentDiagnosticDto& diagnostic) {
            return diagnostic.code == "too-many-extra-pi-env";
        }));
}
