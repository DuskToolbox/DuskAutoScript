#define DAS_BUILD_SHARED

#include "MaapiRunTaskComponent.h"

#include "MaapiExecutionKernel.h"
#include "PluginUtils.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>

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
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        // manifest settings:[] —— 组件无可接受的设置项，空 accepted 是终态
        // 真语义。方法与 {"acceptedSettings": {}} 响应形状保留：GraphRuntime
        // Configure 阶段对每个节点投递 ApplySettingsChange，消费该响应形状。
        auto result = Das::Utils::MakeYyjsonObject();
        (*result.as_object())[std::string_view("acceptedSettings")] =
            Das::Utils::MakeYyjsonObject();
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

        const auto execution_input_json =
            ReadExecutionInputJson(p_input_port_map);
        if (!execution_input_json.has_value())
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        auto envelope_json =
            Das::Utils::ParseYyjsonFromString(*execution_input_json);
        if (!envelope_json.has_value())
        {
            return DAS_E_INVALID_JSON;
        }

        MaaRuntimeResult run_result;
        const DasResult  execute_result = ExecuteEnvelope(
            *envelope_json,
            MaaApiBoundaryForRuntime(),
            stop_token,
            {},
            {},
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

    DasResult MaapiRunTaskComponentFactory::SetTaskComponentHost(
        PluginInterface::IDasTaskComponentHost* p_host)
    {
        host_ = DasPtr<PluginInterface::IDasTaskComponentHost>(p_host);
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
