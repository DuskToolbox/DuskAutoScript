#include "AgentRuntimeRequest.h"

#include <das/Utils/DasJsonCore.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    namespace
    {
        constexpr std::array<std::string_view, 4> kValidCommands{
            "validate",
            "start",
            "stop",
            "status"};

        struct PiEnvKnownField
        {
            std::string_view env_key;
            std::string_view camel_key;
        };

        constexpr std::array<PiEnvKnownField, 8> kKnownPiEnvFields{
            PiEnvKnownField{"PI_INTERFACE_VERSION", "interfaceVersion"},
            PiEnvKnownField{"PI_CLIENT_NAME", "clientName"},
            PiEnvKnownField{"PI_CLIENT_VERSION", "clientVersion"},
            PiEnvKnownField{"PI_CLIENT_LANGUAGE", "clientLanguage"},
            PiEnvKnownField{"PI_CLIENT_MAAFW_VERSION", "clientMaafwVersion"},
            PiEnvKnownField{"PI_VERSION", "projectVersion"},
            PiEnvKnownField{"PI_CONTROLLER", "controllerJson"},
            PiEnvKnownField{"PI_RESOURCE", "resourceJson"}};

        bool IsValidCommand(std::string_view command)
        {
            return std::find(
                       kValidCommands.begin(),
                       kValidCommands.end(),
                       command)
                   != kValidCommands.end();
        }

        bool IsError(const AgentDiagnosticDto& diagnostic)
        {
            return diagnostic.severity == "error";
        }

        void AddDiagnostic(
            ParsedAgentRuntimeRequest& result,
            std::string                severity,
            std::string                code,
            std::string                message,
            std::optional<std::string> path = std::nullopt)
        {
            result.diagnostics.push_back(
                AgentDiagnosticDto{
                    .severity = std::move(severity),
                    .code = std::move(code),
                    .message = std::move(message),
                    .agent_id = std::nullopt,
                    .path = std::move(path)});
        }

        void RefreshOk(ParsedAgentRuntimeRequest& result)
        {
            result.ok = std::none_of(
                result.diagnostics.begin(),
                result.diagnostics.end(),
                IsError);
        }

        template <typename ObjectRef>
        std::optional<std::string> OptionalString(
            const ObjectRef& obj,
            std::string_view key)
        {
            if (!obj.contains(key))
            {
                return std::nullopt;
            }
            const auto& value = obj[key];
            if (!value.is_string())
            {
                return std::nullopt;
            }
            return std::string(value.as_string().value_or(""));
        }

        template <typename ObjectRef>
        std::optional<int32_t> OptionalInt32(
            const ObjectRef& obj,
            std::string_view key)
        {
            if (!obj.contains(key))
            {
                return std::nullopt;
            }
            const auto& value = obj[key];
            auto        parsed = value.as_sint();
            if (!parsed)
            {
                return std::nullopt;
            }
            return static_cast<int32_t>(*parsed);
        }

        template <typename ObjectRef>
        std::optional<bool> OptionalBool(
            const ObjectRef& obj,
            std::string_view key)
        {
            if (!obj.contains(key))
            {
                return std::nullopt;
            }
            const auto& value = obj[key];
            if (!value.is_bool())
            {
                return std::nullopt;
            }
            return value.as_bool().value_or(false);
        }

        bool StartsWithPi(std::string_view key)
        {
            return key.starts_with("PI_");
        }

        bool IsKnownPiEnvKey(std::string_view key)
        {
            return std::any_of(
                kKnownPiEnvFields.begin(),
                kKnownPiEnvFields.end(),
                [key](const PiEnvKnownField& field)
                { return field.env_key == key || field.camel_key == key; });
        }

        void AssignKnownPiEnv(
            PiEnvDto&        env,
            std::string_view key,
            std::string      value)
        {
            if (key == "PI_INTERFACE_VERSION" || key == "interfaceVersion")
            {
                env.interface_version = std::move(value);
            }
            else if (key == "PI_CLIENT_NAME" || key == "clientName")
            {
                env.client_name = std::move(value);
            }
            else if (key == "PI_CLIENT_VERSION" || key == "clientVersion")
            {
                env.client_version = std::move(value);
            }
            else if (key == "PI_CLIENT_LANGUAGE" || key == "clientLanguage")
            {
                env.client_language = std::move(value);
            }
            else if (
                key == "PI_CLIENT_MAAFW_VERSION" || key == "clientMaafwVersion")
            {
                env.client_maafw_version = std::move(value);
            }
            else if (key == "PI_VERSION" || key == "projectVersion")
            {
                env.project_version = std::move(value);
            }
            else if (key == "PI_CONTROLLER" || key == "controllerJson")
            {
                env.controller_json = std::move(value);
            }
            else if (key == "PI_RESOURCE" || key == "resourceJson")
            {
                env.resource_json = std::move(value);
            }
        }

        template <typename ObjectRef>
        void ParseOptionsInto(
            const ObjectRef&        obj,
            AgentRuntimeOptionsDto& options)
        {
            if (auto value = OptionalBool(obj, "tcpCompatMode"))
            {
                options.tcp_compat_mode = *value;
            }
            if (auto value = OptionalBool(obj, "captureOutput"))
            {
                options.capture_output = *value;
            }
            if (auto value = OptionalInt32(obj, "stopTimeoutMs"))
            {
                options.stop_timeout_ms = *value;
            }
            if (auto value = OptionalInt32(obj, "maxOutputTailBytes"))
            {
                options.max_output_tail_bytes = *value;
            }
        }

        template <typename ObjectRef>
        std::vector<std::string> ParseStringArray(
            const ObjectRef& obj,
            std::string_view key)
        {
            std::vector<std::string> result;
            if (!obj.contains(key))
            {
                return result;
            }
            auto array = obj[key].as_array();
            if (!array)
            {
                return result;
            }
            for (auto it = array->begin(); it != array->end(); ++it)
            {
                if (it->is_string())
                {
                    result.emplace_back(it->as_string().value_or(""));
                }
            }
            return result;
        }

        template <typename ObjectRef>
        std::optional<RuntimeRefDto> ParseRuntimeRef(const ObjectRef& obj)
        {
            if (!obj.contains(std::string_view("runtimeRef")))
            {
                return std::nullopt;
            }
            auto runtime_ref = obj[std::string_view("runtimeRef")].as_object();
            if (!runtime_ref)
            {
                return std::nullopt;
            }

            RuntimeRefDto result;
            result.kind = OptionalString(*runtime_ref, "kind").value_or("");
            result.session_id =
                OptionalString(*runtime_ref, "sessionId").value_or("");
            return result;
        }

        template <typename ObjectRef>
        AgentSpecDto ParseAgentSpec(const ObjectRef& obj)
        {
            AgentSpecDto spec;
            spec.child_exec = OptionalString(obj, "childExec").value_or("");
            spec.child_args = ParseStringArray(obj, "childArgs");
            spec.identifier = OptionalString(obj, "identifier");
            if (auto timeout = OptionalInt32(obj, "timeoutMs"))
            {
                spec.timeout_ms = *timeout;
            }
            return spec;
        }

        template <typename ArrayRef>
        void ParseAgentArray(
            const ArrayRef&            array,
            std::vector<AgentSpecDto>& agents)
        {
            for (auto it = array.begin(); it != array.end(); ++it)
            {
                auto agent_obj = it->as_object();
                if (agent_obj)
                {
                    agents.emplace_back(ParseAgentSpec(*agent_obj));
                }
            }
        }

        template <typename ObjectRef>
        void ParseAgents(const ObjectRef& obj, AgentRuntimeRequestDto& request)
        {
            if (obj.contains(std::string_view("agents")))
            {
                auto agents = obj[std::string_view("agents")].as_array();
                if (agents)
                {
                    ParseAgentArray(*agents, request.agent);
                }
                return;
            }

            if (!obj.contains(std::string_view("agent")))
            {
                return;
            }

            const auto& agent = obj[std::string_view("agent")];
            if (auto single = agent.as_object())
            {
                request.agent.emplace_back(ParseAgentSpec(*single));
                return;
            }
            if (auto array = agent.as_array())
            {
                ParseAgentArray(*array, request.agent);
            }
        }

        template <typename ObjectRef>
        void ParsePiEnv(const ObjectRef& obj, ParsedAgentRuntimeRequest& result)
        {
            if (!obj.contains(std::string_view("piEnv")))
            {
                return;
            }
            auto env = obj[std::string_view("piEnv")].as_object();
            if (!env)
            {
                AddDiagnostic(
                    result,
                    "error",
                    "invalid-pi-env",
                    "piEnv must be an object",
                    "piEnv");
                return;
            }

            for (const auto& [key, value] : *env)
            {
                if (!value.is_string())
                {
                    AddDiagnostic(
                        result,
                        "error",
                        "invalid-pi-env-value",
                        "PI environment values must be strings",
                        std::string("piEnv.") + std::string(key));
                    continue;
                }

                auto text = std::string(value.as_string().value_or(""));
                if (IsKnownPiEnvKey(key))
                {
                    AssignKnownPiEnv(
                        result.request.pi_env,
                        key,
                        std::move(text));
                    continue;
                }

                if (!StartsWithPi(key))
                {
                    AddDiagnostic(
                        result,
                        "warning",
                        "ignored-non-pi-env",
                        "Non-PI environment key is ignored for agent launch",
                        std::string("piEnv.") + std::string(key));
                    continue;
                }

                if (result.request.extra_pi_env.size() >= kMaxExtraPiEnv)
                {
                    AddDiagnostic(
                        result,
                        "error",
                        "too-many-extra-pi-env",
                        "Too many unknown PI_* environment extensions",
                        "piEnv");
                    continue;
                }

                result.request.extra_pi_env.emplace_back(
                    PiEnvVarDto{
                        .key = std::string(key),
                        .value = std::move(text)});
            }
        }

        template <typename ObjectRef>
        ParsedAgentRuntimeRequest ParseRequestObject(
            const ObjectRef&                obj,
            AgentRuntimeOptionsDto          base_options,
            std::optional<std::string_view> dispatch_command)
        {
            ParsedAgentRuntimeRequest result;
            result.request.options = base_options;

            if (auto version = OptionalInt32(obj, "version"))
            {
                result.request.version = *version;
            }
            if (result.request.version != 1)
            {
                AddDiagnostic(
                    result,
                    "error",
                    "unsupported-version",
                    "Agent runtime request version is not supported",
                    "version");
            }

            result.request.operation =
                OptionalString(obj, "operation").value_or("");
            if (dispatch_command && result.request.operation.empty())
            {
                result.request.operation = std::string(*dispatch_command);
            }

            if (dispatch_command && !IsValidCommand(*dispatch_command))
            {
                AddDiagnostic(
                    result,
                    "error",
                    "invalid-command",
                    "Dispatch command is not supported",
                    "command");
            }
            if (!IsValidCommand(result.request.operation))
            {
                AddDiagnostic(
                    result,
                    "error",
                    "invalid-command",
                    "Request operation is not supported",
                    "operation");
            }
            if (dispatch_command && IsValidCommand(*dispatch_command)
                && IsValidCommand(result.request.operation)
                && result.request.operation != *dispatch_command)
            {
                AddDiagnostic(
                    result,
                    "error",
                    "command-mismatch",
                    "Dispatch command does not match request operation",
                    "operation");
            }

            result.request.runtime_ref = ParseRuntimeRef(obj);
            result.request.interface_directory =
                OptionalString(obj, "interfaceDirectory").value_or("");
            result.request.session_id = OptionalString(obj, "sessionId");
            result.request.agent_ids = ParseStringArray(obj, "agentIds");
            ParseAgents(obj, result.request);
            ParsePiEnv(obj, result);

            if (obj.contains(std::string_view("options")))
            {
                auto options = obj[std::string_view("options")].as_object();
                if (options)
                {
                    ParseOptionsInto(*options, result.request.options);
                }
                else
                {
                    AddDiagnostic(
                        result,
                        "error",
                        "invalid-options",
                        "options must be an object",
                        "options");
                }
            }

            if (result.request.operation == "start")
            {
                if (result.request.interface_directory.empty())
                {
                    AddDiagnostic(
                        result,
                        "error",
                        "missing-interface-directory",
                        "start requires interfaceDirectory",
                        "interfaceDirectory");
                }
                if (result.request.agent.empty())
                {
                    AddDiagnostic(
                        result,
                        "error",
                        "missing-agent",
                        "start requires at least one agent spec",
                        "agents");
                }
            }

            if (result.request.operation == "start"
                || result.request.operation == "validate")
            {
                for (std::size_t i = 0; i < result.request.agent.size(); ++i)
                {
                    if (result.request.agent[i].child_exec.empty())
                    {
                        AddDiagnostic(
                            result,
                            "error",
                            "missing-child-exec",
                            "agent childExec must not be empty",
                            "agents." + std::to_string(i) + ".childExec");
                    }
                }
            }

            if ((result.request.operation == "stop"
                 || result.request.operation == "status")
                && (!result.request.session_id
                    || result.request.session_id->empty()))
            {
                AddDiagnostic(
                    result,
                    "error",
                    "missing-session-id",
                    "stop/status requires sessionId",
                    "sessionId");
            }

            RefreshOk(result);
            return result;
        }

        std::optional<yyjson::value> ParseJson(std::string_view json)
        {
            return Das::Utils::ParseYyjsonFromString(json);
        }

        AgentRuntimeOptionsDto ParseSettingsOptions(
            std::string_view           settings_json,
            ParsedAgentRuntimeRequest& result)
        {
            AgentRuntimeOptionsDto options;
            if (settings_json.empty())
            {
                return options;
            }

            auto parsed = ParseJson(settings_json);
            if (!parsed)
            {
                AddDiagnostic(
                    result,
                    "error",
                    "invalid-settings-json",
                    "settings_json is not valid JSON");
                return options;
            }
            auto root = parsed->as_object();
            if (!root)
            {
                AddDiagnostic(
                    result,
                    "error",
                    "invalid-settings-json",
                    "settings_json must be a JSON object");
                return options;
            }

            ParseOptionsInto(*root, options);
            if (root->contains(std::string_view("options")))
            {
                auto option_obj =
                    (*root)[std::string_view("options")].as_object();
                if (option_obj)
                {
                    ParseOptionsInto(*option_obj, options);
                }
            }
            return options;
        }

        yyjson::value JsonString(std::string_view value)
        {
            return yyjson::value(std::string(value));
        }

        yyjson::value SerializeDiagnostics(
            const std::vector<AgentDiagnosticDto>& diagnostics)
        {
            auto array = Das::Utils::MakeYyjsonArray();
            auto arr = array.as_array();
            for (const auto& diagnostic : diagnostics)
            {
                auto item = Das::Utils::MakeYyjsonObject();
                auto obj = item.as_object();
                (*obj)[std::string_view("severity")] =
                    JsonString(diagnostic.severity);
                (*obj)[std::string_view("code")] = JsonString(diagnostic.code);
                (*obj)[std::string_view("message")] =
                    JsonString(diagnostic.message);
                if (diagnostic.agent_id)
                {
                    (*obj)[std::string_view("agentId")] =
                        JsonString(*diagnostic.agent_id);
                }
                if (diagnostic.path)
                {
                    (*obj)[std::string_view("path")] =
                        JsonString(*diagnostic.path);
                }
                arr->emplace_back(std::move(item));
            }
            return array;
        }

        yyjson::value SerializeAgentStates(
            const std::vector<AgentStateDto>& agents)
        {
            auto array = Das::Utils::MakeYyjsonArray();
            auto arr = array.as_array();
            for (const auto& agent : agents)
            {
                auto item = Das::Utils::MakeYyjsonObject();
                auto obj = item.as_object();
                (*obj)[std::string_view("agentId")] =
                    JsonString(agent.agent_id);
                (*obj)[std::string_view("state")] = JsonString(agent.state);
                if (agent.identifier)
                {
                    (*obj)[std::string_view("identifier")] =
                        JsonString(*agent.identifier);
                }
                if (agent.pid)
                {
                    (*obj)[std::string_view("pid")] =
                        static_cast<int64_t>(*agent.pid);
                }
                if (agent.exit_code)
                {
                    (*obj)[std::string_view("exitCode")] =
                        static_cast<int64_t>(*agent.exit_code);
                }
                (*obj)[std::string_view("stdoutTail")] =
                    JsonString(agent.stdout_tail);
                (*obj)[std::string_view("stderrTail")] =
                    JsonString(agent.stderr_tail);
                arr->emplace_back(std::move(item));
            }
            return array;
        }

        void AppendEnv(
            std::vector<PiEnvVarDto>& env,
            std::string               key,
            const std::string&        value)
        {
            if (!value.empty())
            {
                env.emplace_back(
                    PiEnvVarDto{.key = std::move(key), .value = value});
            }
        }
    } // namespace

    ParsedAgentRuntimeRequest ParseAgentRuntimeRequest(
        std::string_view request_json)
    {
        auto parsed = ParseJson(request_json);
        if (!parsed)
        {
            ParsedAgentRuntimeRequest result;
            AddDiagnostic(
                result,
                "error",
                "invalid-json",
                "Agent runtime request is not valid JSON");
            RefreshOk(result);
            return result;
        }

        auto root = parsed->as_object();
        if (!root)
        {
            ParsedAgentRuntimeRequest result;
            AddDiagnostic(
                result,
                "error",
                "invalid-request",
                "Agent runtime request must be a JSON object");
            RefreshOk(result);
            return result;
        }

        return ParseRequestObject(
            *root,
            AgentRuntimeOptionsDto{},
            std::nullopt);
    }

    ParsedAgentRuntimeRequest NormalizeAgentRuntimeDispatch(
        std::string_view command,
        std::string_view request_json)
    {
        auto parsed = ParseJson(request_json);
        if (!parsed)
        {
            ParsedAgentRuntimeRequest result;
            AddDiagnostic(
                result,
                "error",
                "invalid-json",
                "Agent runtime request is not valid JSON");
            RefreshOk(result);
            return result;
        }

        auto root = parsed->as_object();
        if (!root)
        {
            ParsedAgentRuntimeRequest result;
            AddDiagnostic(
                result,
                "error",
                "invalid-request",
                "Agent runtime dispatch payload must be a JSON object");
            RefreshOk(result);
            return result;
        }

        return ParseRequestObject(*root, AgentRuntimeOptionsDto{}, command);
    }

    ParsedAgentRuntimeRequest MergeAgentRuntimeSettingsAndInput(
        std::string_view settings_json,
        std::string_view input_json)
    {
        ParsedAgentRuntimeRequest settings_result;
        auto                      base_options =
            ParseSettingsOptions(settings_json, settings_result);
        if (!settings_result.diagnostics.empty())
        {
            RefreshOk(settings_result);
            if (!settings_result.ok)
            {
                return settings_result;
            }
        }

        auto parsed = ParseJson(input_json);
        if (!parsed)
        {
            ParsedAgentRuntimeRequest result = std::move(settings_result);
            AddDiagnostic(
                result,
                "error",
                "invalid-input-json",
                "input_json is not valid JSON");
            RefreshOk(result);
            return result;
        }

        auto root = parsed->as_object();
        if (!root)
        {
            ParsedAgentRuntimeRequest result = std::move(settings_result);
            AddDiagnostic(
                result,
                "error",
                "invalid-input-json",
                "input_json must be a JSON object");
            RefreshOk(result);
            return result;
        }

        auto result = ParseRequestObject(*root, base_options, std::nullopt);
        result.diagnostics.insert(
            result.diagnostics.begin(),
            settings_result.diagnostics.begin(),
            settings_result.diagnostics.end());
        RefreshOk(result);
        return result;
    }

    yyjson::value SerializeAgentRuntimeResult(
        const AgentRuntimeResultDto& result)
    {
        auto root = Das::Utils::MakeYyjsonObject();
        auto obj = root.as_object();
        (*obj)[std::string_view("version")] =
            static_cast<int64_t>(result.version);
        (*obj)[std::string_view("status")] = JsonString(result.status);
        if (result.session_id)
        {
            (*obj)[std::string_view("sessionId")] =
                JsonString(*result.session_id);
        }
        (*obj)[std::string_view("agent")] =
            SerializeAgentStates(result.agent);

        auto outputs = Das::Utils::MakeYyjsonObject();
        auto outputs_obj = outputs.as_object();
        if (result.outputs.agent_session_id)
        {
            (*outputs_obj)[std::string_view("agentSessionId")] =
                JsonString(*result.outputs.agent_session_id);
        }
        (*outputs_obj)[std::string_view("runningAgentCount")] =
            static_cast<int64_t>(result.outputs.running_agent_count);
        (*obj)[std::string_view("outputs")] = std::move(outputs);

        (*obj)[std::string_view("diagnostics")] =
            SerializeDiagnostics(result.diagnostics);

        auto signals = Das::Utils::MakeYyjsonObject();
        auto signals_obj = signals.as_object();
        (*signals_obj)[std::string_view("succeeded")] =
            result.signals.succeeded;
        (*signals_obj)[std::string_view("failed")] = result.signals.failed;
        (*signals_obj)[std::string_view("cancelled")] =
            result.signals.cancelled;
        (*signals_obj)[std::string_view("timedOut")] = result.signals.timed_out;
        (*obj)[std::string_view("signals")] = std::move(signals);
        return root;
    }

    std::string SerializeAgentRuntimeResultJson(
        const AgentRuntimeResultDto& result)
    {
        return Das::Utils::SerializeYyjsonValue(
                   SerializeAgentRuntimeResult(result))
            .value_or("{}");
    }

    std::vector<PiEnvVarDto> BuildLaunchEnvironment(
        const AgentRuntimeRequestDto& request)
    {
        std::vector<PiEnvVarDto> result;
        const auto&              env = request.pi_env;
        AppendEnv(result, "PI_INTERFACE_VERSION", env.interface_version);
        AppendEnv(result, "PI_CLIENT_NAME", env.client_name);
        AppendEnv(result, "PI_CLIENT_VERSION", env.client_version);
        AppendEnv(result, "PI_CLIENT_LANGUAGE", env.client_language);
        AppendEnv(result, "PI_CLIENT_MAAFW_VERSION", env.client_maafw_version);
        AppendEnv(result, "PI_VERSION", env.project_version);
        AppendEnv(result, "PI_CONTROLLER", env.controller_json);
        AppendEnv(result, "PI_RESOURCE", env.resource_json);

        for (const auto& item : request.extra_pi_env)
        {
            if (StartsWithPi(item.key) && !IsKnownPiEnvKey(item.key))
            {
                result.emplace_back(item);
            }
        }
        return result;
    }
} // namespace Das::Plugins::DasMaaPi::AgentRuntime
