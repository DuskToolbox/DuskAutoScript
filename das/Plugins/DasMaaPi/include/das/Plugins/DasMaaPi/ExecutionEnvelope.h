#pragma once

#include <cassert>

#include <cpp_yyjson.hpp>
#include <das/Plugins/DasMaaPi/AgentRuntimeDto.h>
#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/PiDto.h>

#include <optional>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    struct PiEnvSnapshotDto
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

    struct MaaTaskExecutionDto
    {
        std::string   task_name;
        std::string   entry;
        yyjson::value pipeline_override;
    };

    // PI v2.7.0 pretask (D-86-02). External processes executed in order BEFORE
    // CreateController, with CWD = interface_directory. PiCompiler pre-filters
    // by v2.8.1 controller/resource and serializes any selected option values
    // into a trailing single-line compact JSON appended to `args`. MaaRuntime
    // spawns each entry via IPretaskRunner and aborts on launch failure or
    // nonzero exit (DAS_E_MAAPI_PRETASK_FAILED).
    struct MaaPretaskDto
    {
        std::string              name; // optional, for diagnostics
        std::string              exec;
        std::vector<std::string> args; // includes trailing option JSON if any
    };

    struct MaaExecutionPlanDto
    {
        std::string    interface_directory;
        std::string    controller_name;
        ControllerSpec controller;
        // Optional typed init params parallel to `controller`. CompileMaapi
        // fills both: `controller` stays wire-shaped (yyjson round-trip),
        // `controller_init_params` carries win32/gamepad typed bits + display
        // options consumed by CreateControllerFromParams. Adb/dbg controllers
        // leave this nullopt and MaaRuntime::Run uses the legacy
        // single-arg CreateController path.
        std::optional<ControllerInitParams>     controller_init_params;
        std::string                             resource_name;
        std::vector<std::string>                resource_paths;
        std::optional<std::string>              resource_hash;
        bool                                    fail_fast = true;
        bool                                    requires_agent_runtime = false;
        std::vector<AgentRuntime::AgentSpecDto> agent;
        // PI v2.7.0 pretasks. Empty when the project declares no pretask or
        // the v2.8.1 controller/resource filter excluded all entries.
        std::vector<MaaPretaskDto>       pretasks;
        PiEnvSnapshotDto                 pi_env;
        std::vector<MaaTaskExecutionDto> tasks;
    };

    struct ExecutionEnvelopeDto
    {
        int32_t             version = 1;
        std::string         plugin_guid = std::string(kPluginGuidText);
        std::string         task_type_guid = std::string(kTaskGuidText);
        MaaExecutionPlanDto maapi;
    };

    struct CompileSummaryDto
    {
        bool                       can_execute = false;
        std::optional<std::string> controller_name;
        std::optional<std::string> resource_name;
        std::vector<std::string>   task_names;
        bool                       requires_agent_runtime = false;
    };

    struct CompileResultDto
    {
        bool                                ok = true;
        bool                                can_execute = false;
        CompileSummaryDto                   summary;
        std::vector<PiDiagnosticDto>        diagnostics;
        std::optional<ExecutionEnvelopeDto> execution_input;
    };
} // namespace Das::Plugins::DasMaaPi

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::ControllerSpec>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::Win32InitParams>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::GamepadInitParams>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::ControllerInitParams>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiEnvSnapshotDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaaTaskExecutionDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaaPretaskDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaaExecutionPlanDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::ExecutionEnvelopeDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::CompileSummaryDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::CompileResultDto>
{
    using type = yyjson::snake_to_camel_transform;
};
