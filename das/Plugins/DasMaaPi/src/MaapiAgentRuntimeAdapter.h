#ifndef DAS_PLUGINS_DASMAAPI_MAAPIAGENTRUNTIMEADAPTER_H
#define DAS_PLUGINS_DASMAAPI_MAAPIAGENTRUNTIMEADAPTER_H

#include "AgentRuntimeRequest.h"
#include "AgentRuntimeService.h"

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/DasJson.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    AgentRuntime::AgentDiagnosticDto MakeAgentAdapterDiagnostic(
        std::string                code,
        std::string                message,
        std::optional<std::string> path = std::nullopt);

    AgentRuntime::AgentRuntimeResultDto MakeAgentAdapterFailure(
        std::vector<AgentRuntime::AgentDiagnosticDto> diagnostics);

    AgentRuntime::AgentRuntimeResultDto MakeAgentAdapterCancelled();

    AgentRuntime::AgentRuntimeResultDto ExecuteAgentRuntimeRequest(
        AgentRuntime::AgentRuntimeService&             service,
        const AgentRuntime::AgentRuntimeMaaContext&    context,
        const AgentRuntime::ParsedAgentRuntimeRequest& parsed);

    std::string JsonFromDasJson(ExportInterface::IDasJson* json);

    DasPtr<ExportInterface::IDasJson> WrapAgentRuntimeJson(
        const AgentRuntime::AgentRuntimeResultDto& result);
} // namespace Das::Plugins::DasMaaPi

#endif // DAS_PLUGINS_DASMAAPI_MAAPIAGENTRUNTIMEADAPTER_H
