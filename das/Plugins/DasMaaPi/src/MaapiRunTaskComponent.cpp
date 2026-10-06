#define DAS_BUILD_SHARED

#include "MaapiRunTaskComponent.h"

#include "MaapiExecutionKernel.h"
#include "MaapiScheduleBrain.h"
#include "PluginUtils.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>

#include <spdlog/spdlog.h>

#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
        DasResult ReturnJson(
            yyjson::value                     value,
            ExportInterface::IDasJson** const pp_out_result_json)
        {
            auto wrapped = WrapJson(std::move(value));
            if (!wrapped)
            {
                return DAS_E_INVALID_JSON;
            }

            *pp_out_result_json = wrapped.Get();
            (*pp_out_result_json)->AddRef();
            return DAS_S_OK;
        }

        yyjson::value BuildCompletedTasksArray(
            const std::vector<std::string>& completed_tasks)
        {
            auto arr = Das::Utils::MakeYyjsonArray();
            for (const auto& task : completed_tasks)
            {
                arr.as_array()->emplace_back(yyjson::value(std::string{task}));
            }
            return arr;
        }

        // 读取输入口 executionInput 的 envelope JSON 字符串。端口缺失 /
        // 空值返回 nullopt——组件无静默兜底，由调用方显性失败。
        std::optional<std::string> ReadExecutionInputJson(
            ExportInterface::IDasReadOnlyPortMap* p_input_port_map)
        {
            if (p_input_port_map == nullptr)
            {
                return std::nullopt;
            }

            IDasReadOnlyString*     p_value = nullptr;
            const DasReadOnlyString key{"executionInput"};
            if (DAS::IsFailed(p_input_port_map->GetString(key.Get(), &p_value))
                || p_value == nullptr)
            {
                return std::nullopt;
            }

            const char* utf8 = nullptr;
            p_value->GetUtf8(&utf8);
            std::string value(utf8 != nullptr ? utf8 : "");
            p_value->Release();
            if (value.empty())
            {
                return std::nullopt;
            }
            return value;
        }

        // GraphRuntime Configure 阶段的 settings 投递包装：请求体形如
        // {"settings": S}，S 为节点当前配置的全量投影（替换语义）。请求
        // 缺失 / 非对象 / 无 settings 键 / settings 为 null 时按空对象
        // 处理（空投递 = 节点无配置）。
        yyjson::value ReadDeliveredSettings(
            ExportInterface::IDasJson* p_request_json)
        {
            if (p_request_json == nullptr)
            {
                return Das::Utils::MakeYyjsonObject();
            }

            DasPtr<IDasReadOnlyString> request_string;
            if (DAS::IsFailed(p_request_json->ToString(0, request_string.Put()))
                || !request_string)
            {
                return Das::Utils::MakeYyjsonObject();
            }

            const char* request_utf8 = nullptr;
            request_string->GetUtf8(&request_utf8);
            auto parsed = Das::Utils::ParseYyjsonFromString(
                request_utf8 != nullptr ? request_utf8 : "");
            if (!parsed || !parsed->is_object())
            {
                return Das::Utils::MakeYyjsonObject();
            }

            auto request_object = parsed->as_object();
            if (!request_object->contains(std::string_view("settings")))
            {
                return Das::Utils::MakeYyjsonObject();
            }
            const auto& inner = (*request_object)[std::string_view("settings")];
            if (inner.is_null() || !inner.is_object())
            {
                return Das::Utils::MakeYyjsonObject();
            }
            return Das::Utils::CloneYyjsonValue(inner);
        }
    } // namespace

    DasResult MaapiRunTaskComponent::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiRunTaskComponent>();
        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponent::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.RunTaskComponent",
            pp_out_name);
    }

    DasResult MaapiRunTaskComponent::ApplySettingsChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        // manifest config 声明 repositoryRef（type=taskRepositoryRef，值 =
        // 纯 entryId 整数）与 runtimeInputs（object）两键。Configure 投递是
        // 全量替换语义；形状非法整包拒绝（不落半截状态）——项目未发布，
        // 无 legacy 值需要宽容。
        auto                   settings = ReadDeliveredSettings(p_request_json);
        auto                   settings_obj = settings.as_object();
        std::optional<int64_t> accepted_entry_id;
        std::optional<yyjson::value> accepted_runtime_inputs;

        if (settings_obj.has_value()
            && settings_obj->contains(std::string_view("repositoryRef")))
        {
            const auto& repository_ref =
                (*settings_obj)[std::string_view("repositoryRef")];
            if (!repository_ref.is_null())
            {
                const auto entry_id = repository_ref.as_int();
                if (!entry_id.has_value() || *entry_id <= 0)
                {
                    spdlog::error(
                        "[DasMaaPi] ApplySettingsChange: repositoryRef must "
                        "be a positive integer entry id, got non-conforming "
                        "value");
                    return DAS_E_INVALID_ARGUMENT;
                }
                accepted_entry_id = *entry_id;
            }
        }

        if (settings_obj.has_value()
            && settings_obj->contains(std::string_view("runtimeInputs")))
        {
            const auto& runtime_inputs =
                (*settings_obj)[std::string_view("runtimeInputs")];
            if (!runtime_inputs.is_null())
            {
                if (!runtime_inputs.is_object())
                {
                    spdlog::error(
                        "[DasMaaPi] ApplySettingsChange: runtimeInputs must "
                        "be an object");
                    return DAS_E_INVALID_ARGUMENT;
                }
                accepted_runtime_inputs =
                    Das::Utils::CloneYyjsonValue(runtime_inputs);
            }
        }

        {
            const std::scoped_lock lock(settings_mutex_);
            runtime_inputs_ =
                accepted_runtime_inputs.has_value()
                    ? Das::Utils::CloneYyjsonValue(*accepted_runtime_inputs)
                    : yyjson::value{};
        }

        // acceptedSettings 回显本轮实际接受的键（存储值）。
        auto accepted = Das::Utils::MakeYyjsonObject();
        auto accepted_object = accepted.as_object();
        if (accepted_entry_id.has_value())
        {
            (*accepted_object)[std::string_view("repositoryRef")] =
                *accepted_entry_id;
        }
        if (accepted_runtime_inputs.has_value())
        {
            (*accepted_object)[std::string_view("runtimeInputs")] =
                Das::Utils::CloneYyjsonValue(*accepted_runtime_inputs);
        }

        auto result = Das::Utils::MakeYyjsonObject();
        (*result.as_object())[std::string_view("acceptedSettings")] =
            std::move(accepted);
        return ReturnJson(std::move(result), pp_out_result_json);
    }

    DasResult MaapiRunTaskComponent::Do(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map)
    {
        if (pp_out_port_map == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_port_map = nullptr;

        // envelope 来源唯一：executionInput 端口喂线（编排层在节点执行
        // 时现编、产物经输入口送达）。端口缺失（含 settings 携带
        // repositoryRef 但口未喂线的情形）遇缺即错：具名失败码，运行期零
        // 自解析回退、零主进程服务查询。
        const auto execution_input_json =
            ReadExecutionInputJson(p_input_port_map);
        if (!execution_input_json.has_value())
        {
            spdlog::error(
                "[DasMaaPi] Do: executionInput port is missing — the "
                "envelope must be delivered by the orchestration layer's "
                "execution-time compile");
            return DAS_E_MAAPI_EXECUTION_INPUT_MISSING;
        }
        const std::string envelope_source = std::move(*execution_input_json);

        // settings 的 runtimeInputs 运行时输入覆盖与 envelope 来源无关：
        // 键恒等映射进 envelope 运行时输入段（maapi.tasks[].pipelineOverride
        // 的对象级覆盖）——复用执行内核既有的传参通道。
        yyjson::value runtime_inputs;
        {
            const std::scoped_lock lock(settings_mutex_);
            runtime_inputs = Das::Utils::CloneYyjsonValue(runtime_inputs_);
        }

        auto envelope_json = Das::Utils::ParseYyjsonFromString(envelope_source);
        if (!envelope_json.has_value())
        {
            return DAS_E_INVALID_JSON;
        }

        // runtimeInputs 键恒等映射进执行内核传参通道（语义见读取处注释）。
        std::map<std::string, std::string>   runtime_input_port_map;
        std::map<std::string, yyjson::value> runtime_input_values;
        if (auto runtime_inputs_object = runtime_inputs.as_object())
        {
            for (const auto& [input_name, input_value] : *runtime_inputs_object)
            {
                runtime_input_port_map.emplace(
                    std::string(input_name),
                    std::string(input_name));
                runtime_input_values.emplace(
                    std::string(input_name),
                    Das::Utils::CloneYyjsonValue(input_value));
            }
        }

        MaaRuntimeResult run_result;
        const DasResult  execute_result = ExecuteEnvelope(
            *envelope_json,
            MaaApiBoundaryForRuntime(),
            stop_token,
            runtime_input_port_map,
            runtime_input_values,
            run_result);

        DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        auto completed_tasks_array =
            BuildCompletedTasksArray(run_result.completed_tasks);
        auto completed_serialized =
            Das::Utils::SerializeYyjsonValue(completed_tasks_array);
        if (completed_serialized)
        {
            output_map->SetString(
                DasReadOnlyString("completedTasks").Get(),
                DasReadOnlyString(completed_serialized->c_str()).Get());
        }

        // stopped 语义：内核 stop 前置短路返回 DAS_E_TIMEOUT；运行中取消由
        // MaaRuntime 置 stopped 标志（das_result 为 DAS_E_FAIL）——两者都算停。
        output_map->SetBool(
            DasReadOnlyString("stopped").Get(),
            run_result.stopped || run_result.das_result == DAS_E_TIMEOUT);

        if (!run_result.diagnostics.empty())
        {
            auto diagnostics_array = Das::Utils::MakeYyjsonArray();
            for (const auto& diagnostic : run_result.diagnostics)
            {
                auto diagnostic_object = Das::Utils::MakeYyjsonObject();
                auto diagnostic_fields = diagnostic_object.as_object();
                (*diagnostic_fields)[std::string_view("severity")] =
                    std::make_pair(
                        std::string_view(diagnostic.severity),
                        yyjson::copy_string);
                (*diagnostic_fields)[std::string_view("code")] = std::make_pair(
                    std::string_view(diagnostic.code),
                    yyjson::copy_string);
                (*diagnostic_fields)[std::string_view("message")] =
                    std::make_pair(
                        std::string_view(diagnostic.message),
                        yyjson::copy_string);
                diagnostics_array.as_array()->emplace_back(
                    std::move(diagnostic_object));
            }
            auto diagnostics_serialized =
                Das::Utils::SerializeYyjsonValue(diagnostics_array);
            if (diagnostics_serialized)
            {
                output_map->SetString(
                    DasReadOnlyString("diagnostics").Get(),
                    DasReadOnlyString(diagnostics_serialized->c_str()).Get());
            }
        }
        else
        {
            output_map->SetString(
                DasReadOnlyString("diagnostics").Get(),
                DasReadOnlyString("[]").Get());
        }

        // D-B5 nextTime 默认输出口：值 = 共享 schedule brain（101 先例延续，
        // 插件自治）算出的下次执行时间，ISO 8601 UTC 定长 19 字符（111-01
        // wire 契约，111-02 adapter 按同 id 捕获，连到 end「下次时间」口）。
        // 本次 Do 刚执行（今日已 fired）→ daily 恒推明日 slot，与 MaapiTask
        // Do→GetNextExecutionTime 序列同语义。engaged-only 纪律（Q4 裁决）：
        // schedule=off / 描述非法 → 不发键，adapter 捕获不到自然回落静态
        // 通道；执行失败不发（成功路径末尾）。
        if (!DAS::IsFailed(execute_result))
        {
            if (auto parsed_envelope = ParseExecutionEnvelope(*envelope_json);
                !DAS::IsFailed(parsed_envelope.result))
            {
                const std::time_t now = now_();
                const auto        next = ComputeNextScheduleRun(
                    parsed_envelope.envelope.maapi.schedule,
                    now,
                    EncodeLocalYmd(now));
                if (next.status == ScheduleNextStatus::Ok)
                {
                    output_map->SetString(
                        DasReadOnlyString("nextTime").Get(),
                        DasReadOnlyString(
                            FormatNextRunIso8601Utc(next.next_unix).c_str())
                            .Get());
                }
            }
        }

        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();

        if (DAS::IsFailed(execute_result))
        {
            return execute_result;
        }

        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponentFactory::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiRunTaskComponentFactory>();
        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponentFactory::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.RunTaskComponentFactory",
            pp_out_name);
    }

    DasResult MaapiRunTaskComponentFactory::CreateComponent(
        const DasGuid&                       component_guid,
        PluginInterface::IDasTaskComponent** pp_out_component)
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;
        if (component_guid != DasIidOf<MaapiRunTaskComponent>())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto* component = new MaapiRunTaskComponent();
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
