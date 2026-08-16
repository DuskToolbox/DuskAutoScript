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

    /// 事件日志行草案：type = 条目级别，text = 单行文本（[code] 前缀 +
    /// i18n 翻译 + provider_code 追加）。
    struct RunLogLine
    {
        Das::ExportInterface::DasLogType type;
        std::string                      text;
    };

    /// 由 MaaRuntimeResult 生成事件日志行序列（纯函数）：失败 → 标题行
    /// （ERROR，含错误码）+ 全部诊断行；成功 → 仅 warning 诊断行（error 级
    /// 只伴随失败出现）。行内 i18n 模板 / {0} 原文回退 / provider_code
    /// 追加规则见实现。
    std::vector<RunLogLine> BuildRunLogLines(const MaaRuntimeResult& result);

    /// Run 结果的事件日志外显：诊断此前随 MaaRuntimeResult 在调用方被丢弃
    /// （上抛的只有裸 das_result，调试日志与事件日志都看不到原因，MaaFW
    /// 早期失败时连 MAA 回调日志都没有）。BuildRunLogLines 逐行建独立
    /// focus 条目写 user.jsonl（事件日志面板可见），severity 保真——warning
    /// 诊断是 WARNING 条目，不再垫进 ERROR 条目文本；无 recorder 时整体
    /// [focus-degraded] 降级落调试日志，Record 失败由 RecordEntryOrLog
    /// 按行降级。
    void LogRunResult(const MaaRuntimeResult& result);

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
