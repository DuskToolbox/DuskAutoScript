#pragma once

#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/ExecutionEnvelope.h>
#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>
#include <das/_autogen/idl/abi/IDasTask.h>

#include <optional>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    struct MaaRuntimeDiagnostic
    {
        std::string                severity;
        std::string                code;
        std::string                message;
        std::optional<std::int64_t> provider_code;
    };

    struct MaaRuntimeResult
    {
        DasResult                         das_result = DAS_S_OK;
        bool                              stopped = false;
        std::vector<std::string>          completed_tasks;
        std::vector<MaaRuntimeDiagnostic> diagnostics;
    };

    struct ParsedExecutionEnvelope
    {
        DasResult            result = DAS_S_OK;
        ExecutionEnvelopeDto envelope;
        std::string          message;
    };

    ParsedExecutionEnvelope ParseExecutionEnvelope(
        const yyjson::value& value);

    DasResult ValidateExecutionEnvelope(
        const ExecutionEnvelopeDto& envelope);

    class MaaRuntime
    {
    public:
        static MaaRuntimeResult Run(
            const ExecutionEnvelopeDto&        envelope,
            IMaaApiBoundary&                   boundary,
            PluginInterface::IDasStopToken*    stop_token);
    };

    void SetMaaApiBoundaryForTest(IMaaApiBoundary* boundary);
    IMaaApiBoundary& MaaApiBoundaryForRuntime();
} // namespace Das::Plugins::DasMaaPi
