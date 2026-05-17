#define DAS_BUILD_SHARED

#include "MaapiAgentRuntimeAdapter.h"

#include "AgentRuntimeMaaContextResolver.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>

#include <utility>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        bool IsStartStopStatus(std::string_view operation)
        {
            return operation == "start" || operation == "stop"
                   || operation == "status";
        }
    } // namespace

    AgentRuntime::AgentDiagnosticDto MakeAgentAdapterDiagnostic(
        std::string                code,
        std::string                message,
        std::optional<std::string> path)
    {
        return AgentRuntime::AgentDiagnosticDto{
            .severity = "error",
            .code = std::move(code),
            .message = std::move(message),
            .agent_id = std::nullopt,
            .path = std::move(path)};
    }

    AgentRuntime::AgentRuntimeResultDto MakeAgentAdapterFailure(
        std::vector<AgentRuntime::AgentDiagnosticDto> diagnostics)
    {
        AgentRuntime::AgentRuntimeResultDto result;
        result.status = "failed";
        result.diagnostics = std::move(diagnostics);
        result.signals.failed = true;
        result.signals.succeeded = false;
        return result;
    }

    AgentRuntime::AgentRuntimeResultDto MakeAgentAdapterCancelled()
    {
        AgentRuntime::AgentRuntimeResultDto result;
        result.status = "cancelled";
        result.signals.cancelled = true;
        result.signals.succeeded = false;
        result.signals.failed = false;
        return result;
    }

    AgentRuntime::AgentRuntimeResultDto ExecuteAgentRuntimeRequest(
        AgentRuntime::AgentRuntimeService&             service,
        const AgentRuntime::AgentRuntimeMaaContext&    context,
        const AgentRuntime::ParsedAgentRuntimeRequest& parsed)
    {
        if (!parsed.ok)
        {
            return MakeAgentAdapterFailure(parsed.diagnostics);
        }

        const auto& request = parsed.request;
        if (request.operation == "validate")
        {
            AgentRuntime::AgentRuntimeResultDto result;
            result.status = "succeeded";
            result.diagnostics = parsed.diagnostics;
            result.signals.succeeded = true;
            return result;
        }
        if (request.operation == "start")
        {
            auto resolved_context = context;
            if (!AgentRuntime::IsUsableMaaContext(resolved_context))
            {
                if (!request.runtime_ref)
                {
                    return MakeAgentAdapterFailure({MakeAgentAdapterDiagnostic(
                        "missing-runtime-ref",
                        "start requires runtimeRef when the adapter has no "
                        "injected Maa runtime context",
                        "runtimeRef")});
                }

                auto resolved =
                    AgentRuntime::ResolveMaaContext(*request.runtime_ref);
                if (!resolved)
                {
                    return MakeAgentAdapterFailure({MakeAgentAdapterDiagnostic(
                        "runtime-ref-not-found",
                        "runtimeRef does not reference an active Maa "
                        "runtime session",
                        "runtimeRef")});
                }
                resolved_context = *resolved;
            }
            return service.Start(request, resolved_context);
        }
        if (request.operation == "stop")
        {
            return service.Stop(*request.session_id, request.options);
        }
        if (request.operation == "status")
        {
            return service.Status(*request.session_id);
        }

        return MakeAgentAdapterFailure({MakeAgentAdapterDiagnostic(
            "invalid-command",
            IsStartStopStatus(request.operation)
                ? "Agent runtime command is not supported by this adapter"
                : "Agent runtime operation is not supported",
            "operation")});
    }

    std::string JsonFromDasJson(ExportInterface::IDasJson* json)
    {
        if (json == nullptr)
        {
            return {};
        }

        DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return {};
        }

        const char* raw = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&raw)) || raw == nullptr)
        {
            return {};
        }
        return raw;
    }

    DasPtr<ExportInterface::IDasJson> WrapAgentRuntimeJson(
        const AgentRuntime::AgentRuntimeResultDto& result)
    {
        auto json = AgentRuntime::SerializeAgentRuntimeResultJson(result);
        DasPtr<ExportInterface::IDasJson> wrapped;
        ParseDasJsonFromString(json.c_str(), wrapped.Put());
        return wrapped;
    }
} // namespace Das::Plugins::DasMaaPi
