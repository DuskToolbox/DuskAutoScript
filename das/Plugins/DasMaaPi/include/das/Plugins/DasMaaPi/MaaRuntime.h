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
    namespace AgentRuntime
    {
        class IAgentProcessRunner;
    }

    struct MaaRuntimeDiagnostic
    {
        std::string                 severity;
        std::string                 code;
        std::string                 message;
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

    ParsedExecutionEnvelope ParseExecutionEnvelope(const yyjson::value& value);

    DasResult ValidateExecutionEnvelope(const ExecutionEnvelopeDto& envelope);

    /// Run 失败后的诊断外显：diagnostics 此前随 MaaRuntimeResult 在调用方
    /// 被丢弃（上抛的只有裸 das_result，调试日志与事件日志都看不到失败
    /// 原因，MaaFW 早期失败时连 MAA 回调日志都没有）。打 spdlog ERROR +
    /// 写一条 ERROR focus 条目（user.jsonl → 事件日志面板）；recorder
    /// resolve 失败由 RecordEntryOrLog 降级 [focus-degraded] 落调试日志。
    void LogRunFailure(const MaaRuntimeResult& result);

    class MaaRuntime
    {
    public:
        static MaaRuntimeResult Run(
            const ExecutionEnvelopeDto&     envelope,
            IMaaApiBoundary&                boundary,
            PluginInterface::IDasStopToken* stop_token);
    };

    void SetMaaApiBoundaryForTest(IMaaApiBoundary* boundary);
    void SetAgentProcessRunnerForTest(
        AgentRuntime::IAgentProcessRunner* runner);
    AgentRuntime::IAgentProcessRunner* AgentProcessRunnerForRuntime();
    IMaaApiBoundary&                   MaaApiBoundaryForRuntime();
} // namespace Das::Plugins::DasMaaPi
