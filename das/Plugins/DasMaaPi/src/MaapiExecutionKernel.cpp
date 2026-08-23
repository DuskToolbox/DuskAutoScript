#include "MaapiExecutionKernel.h"

#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>

#include <spdlog/spdlog.h>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
        bool IsStopRequested(PluginInterface::IDasStopToken* stop_token)
        {
            if (!stop_token)
            {
                return false;
            }
            bool can_stop = false;
            return DAS::IsOk(stop_token->StopRequested(&can_stop)) && can_stop;
        }

        // graph 端口 → PI 参数的运行期传参合并：port_map 把 graph_port_id
        // 映射到 pipeline_override 的 pi_param_name 键，inputs 携带各
        // graph_port_id 的值。port_map 或 inputs 任一为空即空操作（通道 B
        // 运行期传参接线前，两侧调用方恒传空表）。
        void MergePortMapInputs(
            ExecutionEnvelopeDto&                      envelope,
            const std::map<std::string, std::string>&  port_map,
            const std::map<std::string, yyjson::value> inputs)
        {
            if (port_map.empty() || inputs.empty())
            {
                return;
            }

            for (auto& task : envelope.maapi.tasks)
            {
                auto pipeline_obj = task.pipeline_override.as_object();
                if (!pipeline_obj)
                {
                    task.pipeline_override = Das::Utils::MakeYyjsonObject();
                    pipeline_obj = task.pipeline_override.as_object();
                    if (!pipeline_obj)
                    {
                        continue;
                    }
                }

                for (const auto& [graph_port_id, pi_param_name] : port_map)
                {
                    auto it = inputs.find(graph_port_id);
                    if (it == inputs.end())
                    {
                        continue;
                    }

                    const auto& input_value = it->second;
                    (*pipeline_obj)[pi_param_name] =
                        Das::Utils::CloneYyjsonValue(input_value);
                }
            }
        }
    } // namespace

    DasResult ExecuteEnvelope(
        const yyjson::value&                        envelope_json,
        IMaaApiBoundary&                            boundary,
        PluginInterface::IDasStopToken*             stop_token,
        const std::map<std::string, std::string>&   port_map,
        const std::map<std::string, yyjson::value>& inputs,
        MaaRuntimeResult&                           out_result)
    {
        out_result = MaaRuntimeResult{};

        auto parsed = ParseExecutionEnvelope(envelope_json);
        if (DAS::IsFailed(parsed.result))
        {
            spdlog::error(
                "[DasMaaPi] ExecuteEnvelope: envelope parse failed, "
                "result={}, message={}",
                parsed.result,
                parsed.message);
            out_result.das_result = parsed.result;
            return parsed.result;
        }

        MergePortMapInputs(parsed.envelope, port_map, inputs);

        if (IsStopRequested(stop_token))
        {
            out_result.das_result = DAS_E_TIMEOUT;
            return DAS_E_TIMEOUT;
        }

        out_result = MaaRuntime::Run(parsed.envelope, boundary, stop_token);
        // 结果诊断外显（事件日志）：warning 级无论成败投递（sink 注册降级
        // 等在成功运行也会出现），标题与 error 级仅失败时投递。
        LogRunResult(out_result);
        return out_result.das_result;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
