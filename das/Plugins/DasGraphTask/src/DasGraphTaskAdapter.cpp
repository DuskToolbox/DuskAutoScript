#define DAS_BUILD_SHARED

#include <das/Plugins/DasGraphTask/DasGraphTaskAdapter.h>

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>
#include <das/Utils/TaskComponentPorts.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    DasGraphTaskAdapter::DasGraphTaskAdapter(
        PluginInterface::IDasTaskComponentHost* p_host)
        : host_(DasPtr<PluginInterface::IDasTaskComponentHost>(p_host))
    {
    }

    DasResult DasGraphTaskAdapter::Do(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*      p_environment_json,
        ExportInterface::IDasJson*      p_task_settings_json)
    {
        std::ignore = p_environment_json;

        // task_settings is the CompiledGraphPlanDto serialization the
        // scheduler extracted from the authoring compile envelope's
        // compile.executionInput — pure pass-through, no field extraction.
        DAS::DasPtr<IDasReadOnlyString> artifact;
        if (p_task_settings_json != nullptr)
        {
            const DasResult to_string_hr =
                p_task_settings_json->ToString(0, artifact.Put());
            if (DAS::IsFailed(to_string_hr))
            {
                return DAS_E_INVALID_JSON;
            }
        }

        // 形状适配：IDasTask 的 settings JSON → 内核组件的 compiledPlan 输入
        // 口（task_settings 为 null 时不设口，保持 null artifact 语义）。
        DAS::DasPtr<ExportInterface::IDasPortMap> input_map;
        const auto create_map_hr = CreateIDasPortMap(input_map.Put());
        if (DAS::IsFailed(create_map_hr))
        {
            return create_map_hr;
        }
        if (artifact.Get() != nullptr)
        {
            const char* plan_utf8 = nullptr;
            const auto  get_hr = artifact->GetUtf8(&plan_utf8);
            if (DAS::IsFailed(get_hr))
            {
                return get_hr;
            }
            if (plan_utf8 == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            Das::Utils::SetPortString(
                input_map.Get(),
                "compiledPlan",
                plan_utf8);
        }

        // 委托同模块直接构造的组件内核执行（GraphRuntime 驱动代码仅
        // DasGraphTaskImpl 一份）；输出 portmap 丢弃——IDasTask 无结构化
        // 返回值（fire-and-forget，与 MaapiTask 同语义）。无 host 时内核
        // 自身以结构化错误失败，hostless 回退分支随之消亡。
        DasGraphTaskImpl                          component(host_.Get());
        DAS::DasPtr<ExportInterface::IDasPortMap> output;
        return component.Do(stop_token, input_map.Get(), output.Put());
    }

    DasResult DasGraphTaskAdapter::GetNextExecutionTime(
        ExportInterface::IDasJson* /*p_task_settings_json*/,
        ExportInterface::DasDate* p_out_date)
    {
        if (p_out_date == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return DAS_E_NO_IMPLEMENTATION;
    }
} // namespace Plugins::DasGraphTask
DAS_NS_END
