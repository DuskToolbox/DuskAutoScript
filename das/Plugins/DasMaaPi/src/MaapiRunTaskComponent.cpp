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
#include <das/_autogen/idl/abi/IDasStringVector.h>

#include <spdlog/spdlog.h>

#include <cstdint>
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

        // 读单口值并转 JSON 值形态（标量四类——PI 参数的值域）。image/
        // base/component 无 JSON 值形态、json/signal 非调用方输入面，一律
        // 返回 nullopt 由调用方跳过（与图侧七类分派抄写的可转移集合对齐）。
        std::optional<yyjson::value> ReadPortValueAsOverrideJson(
            ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            const std::string&                    port_id)
        {
            const DasReadOnlyString key{port_id.c_str()};

            ExportInterface::DasVariantType kind =
                ExportInterface::DAS_VARIANT_TYPE_NULL;
            if (DAS::IsFailed(p_input_port_map->GetType(key.Get(), &kind)))
            {
                return std::nullopt;
            }

            switch (kind)
            {
            case ExportInterface::DAS_VARIANT_TYPE_INT:
            {
                int64_t value = 0;
                if (DAS::IsFailed(p_input_port_map->GetInt(key.Get(), &value)))
                {
                    return std::nullopt;
                }
                return yyjson::value(value);
            }
            case ExportInterface::DAS_VARIANT_TYPE_FLOAT:
            {
                double value = 0.0;
                if (DAS::IsFailed(
                        p_input_port_map->GetFloat(key.Get(), &value)))
                {
                    return std::nullopt;
                }
                return yyjson::value(value);
            }
            case ExportInterface::DAS_VARIANT_TYPE_BOOL:
            {
                bool value = false;
                if (DAS::IsFailed(p_input_port_map->GetBool(key.Get(), &value)))
                {
                    return std::nullopt;
                }
                return yyjson::value(value);
            }
            case ExportInterface::DAS_VARIANT_TYPE_STRING:
            {
                IDasReadOnlyString* p_value = nullptr;
                if (DAS::IsFailed(
                        p_input_port_map->GetString(key.Get(), &p_value))
                    || p_value == nullptr)
                {
                    return std::nullopt;
                }
                const char* utf8 = nullptr;
                p_value->GetUtf8(&utf8);
                yyjson::value value(std::string(utf8 != nullptr ? utf8 : ""));
                p_value->Release();
                return value;
            }
            default:
            {
                spdlog::warn(
                    "[DasMaaPi] Do: input port '{}' carries a non-scalar "
                    "value (variant kind {}); skipping override mapping",
                    port_id,
                    static_cast<int>(kind));
                return std::nullopt;
            }
            }
        }

        // 输入 portmap 的非保留键 = 暴露口（键 = 口 id = PI 参数名），值 =
        // 暴露口连线值/边界默认（编排层绑定抄写落 portmap）。恒等映射进
        // 执行内核传参通道（maapi.tasks[].pipelineOverride 的对象级覆盖）
        // ——执行期参数覆盖两层语义：暴露口连线值 > entry 编译期选项值，
        // 口未连线 = 编译期值生效，无第三来源。保留键 executionInput 不参
        // 与（产物运输专用）。
        void CollectWiredExposedInputs(
            ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            std::map<std::string, std::string>&   port_map,
            std::map<std::string, yyjson::value>& inputs)
        {
            if (p_input_port_map == nullptr)
            {
                return;
            }

            DasPtr<ExportInterface::IDasStringVector> keys;
            if (DAS::IsFailed(p_input_port_map->GetKeys(keys.Put())) || !keys)
            {
                return;
            }

            std::uint64_t count = 0;
            if (DAS::IsFailed(keys->Size(&count)))
            {
                return;
            }

            for (std::uint64_t i = 0; i < count; ++i)
            {
                IDasReadOnlyString* p_key = nullptr;
                if (DAS::IsFailed(keys->At(i, &p_key)) || p_key == nullptr)
                {
                    continue;
                }
                const char* key_utf8 = nullptr;
                p_key->GetUtf8(&key_utf8);
                const std::string port_id(key_utf8 != nullptr ? key_utf8 : "");
                p_key->Release();
                if (port_id.empty() || port_id == "executionInput")
                {
                    continue;
                }

                auto value =
                    ReadPortValueAsOverrideJson(p_input_port_map, port_id);
                if (!value.has_value())
                {
                    continue;
                }
                port_map.emplace(port_id, port_id);
                inputs.emplace(port_id, std::move(*value));
            }
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
        // 纯 entryId 整数）单键。Configure 投递是全量替换语义；形状非法
        // 整包拒绝（不落半截状态）——项目未发布，无 legacy 值需要宽容。
        auto                   settings = ReadDeliveredSettings(p_request_json);
        auto                   settings_obj = settings.as_object();
        std::optional<int64_t> accepted_entry_id;

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

        // acceptedSettings 回显本轮实际接受的键（存储值）。
        auto accepted = Das::Utils::MakeYyjsonObject();
        auto accepted_object = accepted.as_object();
        if (accepted_entry_id.has_value())
        {
            (*accepted_object)[std::string_view("repositoryRef")] =
                *accepted_entry_id;
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

        auto envelope_json = Das::Utils::ParseYyjsonFromString(envelope_source);
        if (!envelope_json.has_value())
        {
            return DAS_E_INVALID_JSON;
        }

        // 执行期参数覆盖唯一入口 = 暴露口：输入 portmap 非保留键恒等映射
        // 进执行内核传参通道（语义见 CollectWiredExposedInputs 注释）。
        std::map<std::string, std::string>   runtime_input_port_map;
        std::map<std::string, yyjson::value> runtime_input_values;
        CollectWiredExposedInputs(
            p_input_port_map,
            runtime_input_port_map,
            runtime_input_values);

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
