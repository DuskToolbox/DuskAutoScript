#pragma once

#include <das/Plugins/DasMaaPi/AgentRuntimeDto.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    inline constexpr std::size_t kMaxExtraPiEnv = 16;

    struct ParsedAgentRuntimeRequest
    {
        bool                            ok = false;
        AgentRuntimeRequestDto          request;
        std::vector<AgentDiagnosticDto> diagnostics;
    };

    ParsedAgentRuntimeRequest ParseAgentRuntimeRequest(
        std::string_view request_json);

    ParsedAgentRuntimeRequest NormalizeAgentRuntimeDispatch(
        std::string_view command,
        std::string_view request_json);

    ParsedAgentRuntimeRequest MergeAgentRuntimeSettingsAndInput(
        std::string_view settings_json,
        std::string_view input_json);

    yyjson::value SerializeAgentRuntimeResult(
        const AgentRuntimeResultDto& result);

    std::string SerializeAgentRuntimeResultJson(
        const AgentRuntimeResultDto& result);

    std::vector<PiEnvVarDto> BuildLaunchEnvironment(
        const AgentRuntimeRequestDto& request);
} // namespace Das::Plugins::DasMaaPi::AgentRuntime
