#pragma once

#include <cassert>

#include <cpp_yyjson.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    struct RuntimeRefDto
    {
        std::string kind;
        std::string session_id;
    };

    struct AgentSpecDto
    {
        std::string                child_exec;
        std::vector<std::string>   child_args;
        std::optional<std::string> identifier;
        int32_t                    timeout_ms = -1;
    };

    struct PiEnvDto
    {
        std::string interface_version = "2";
        std::string client_name = "DAS";
        std::string client_version;
        std::string client_language = "cpp";
        std::string client_maafw_version;
        std::string project_version;
        std::string controller_json;
        std::string resource_json;
    };

    using PiEnv = PiEnvDto;

    struct PiEnvVarDto
    {
        std::string key;
        std::string value;
    };

    struct AgentRuntimeOptionsDto
    {
        bool    tcp_compat_mode = false;
        bool    capture_output = true;
        int32_t stop_timeout_ms = 5000;
        int32_t max_output_tail_bytes = 16384;
    };

    struct AgentRuntimeSettingsDto
    {
        AgentRuntimeOptionsDto options;
    };

    struct AgentRuntimeInputDto
    {
        std::string                  operation;
        std::optional<RuntimeRefDto> runtime_ref;
        std::string                  interface_directory;
        std::vector<AgentSpecDto>    agents;
        PiEnvDto                     pi_env;
        std::vector<PiEnvVarDto>     extra_pi_env;
        std::optional<std::string>   session_id;
        std::vector<std::string>     agent_ids;
        AgentRuntimeOptionsDto       options;
    };

    struct AgentRuntimeRequestDto
    {
        int32_t                      version = 1;
        std::string                  operation;
        std::optional<RuntimeRefDto> runtime_ref;
        std::string                  interface_directory;
        std::vector<AgentSpecDto>    agents;
        PiEnvDto                     pi_env;
        std::vector<PiEnvVarDto>     extra_pi_env;
        AgentRuntimeOptionsDto       options;
        std::optional<std::string>   session_id;
        std::vector<std::string>     agent_ids;
    };

    struct AgentDiagnosticDto
    {
        std::string                severity;
        std::string                code;
        std::string                message;
        std::optional<std::string> agent_id;
        std::optional<std::string> path;
    };

    struct AgentStateDto
    {
        std::string                agent_id;
        std::string                state;
        std::optional<std::string> identifier;
        std::optional<uint32_t>    pid;
        std::optional<int32_t>     exit_code;
        std::string                stdout_tail;
        std::string                stderr_tail;
    };

    struct AgentRuntimeOutputsDto
    {
        std::optional<std::string> agent_session_id;
        int32_t                    running_agent_count = 0;
    };

    struct AgentRuntimeSignalsDto
    {
        bool succeeded = false;
        bool failed = false;
        bool cancelled = false;
        bool timed_out = false;
    };

    struct AgentRuntimeResultDto
    {
        int32_t                         version = 1;
        std::string                     status;
        std::optional<std::string>      session_id;
        std::vector<AgentStateDto>      agents;
        AgentRuntimeOutputsDto          outputs;
        std::vector<AgentDiagnosticDto> diagnostics;
        AgentRuntimeSignalsDto          signals;
    };
} // namespace Das::Plugins::DasMaaPi::AgentRuntime

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::RuntimeRefDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::AgentSpecDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::AgentRuntime::PiEnvDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::PiEnvVarDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::AgentRuntimeOptionsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::AgentRuntimeSettingsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::AgentRuntimeInputDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::AgentRuntimeRequestDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::AgentDiagnosticDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::AgentStateDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::AgentRuntimeOutputsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::AgentRuntimeSignalsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::AgentRuntime::AgentRuntimeResultDto>
{
    using type = yyjson::snake_to_camel_transform;
};
