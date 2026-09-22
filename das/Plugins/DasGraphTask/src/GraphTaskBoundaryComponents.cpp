#include "GraphTaskBoundaryComponents.h"

#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/header/IDasStringVector.generated.h>

#include <cstdint>
#include <string>
#include <utility>

namespace Das::Plugins::DasGraphTask
{
    namespace
    {
        // 逐键类型分派的同名转写：读侧按 DasVariantType 分派 Get*，写侧
        // 同名 Set*（[out] 出参一律 DasPtr.Put() 接、Set* 传 .Get() 裸指
        // 针）。NULL 无数据、SIGNAL 是运行期控制形态，不转写；JSON 以文
        // 本形态经 GetJson→SetString 透传。
        DasResult TransposePortEntry(
            Das::ExportInterface::IDasReadOnlyPortMap* input_map,
            Das::ExportInterface::IDasPortMap*         output_map,
            const std::string&                         port_id)
        {
            using Das::ExportInterface::DAS_VARIANT_TYPE_BASE;
            using Das::ExportInterface::DAS_VARIANT_TYPE_BOOL;
            using Das::ExportInterface::DAS_VARIANT_TYPE_COMPONENT;
            using Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT;
            using Das::ExportInterface::DAS_VARIANT_TYPE_IMAGE;
            using Das::ExportInterface::DAS_VARIANT_TYPE_INT;
            using Das::ExportInterface::DAS_VARIANT_TYPE_JSON;
            using Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
            using Das::ExportInterface::DAS_VARIANT_TYPE_SIGNAL;
            using Das::ExportInterface::DAS_VARIANT_TYPE_STRING;
            using Das::ExportInterface::DasVariantType;

            const DasReadOnlyString key{port_id.c_str()};

            DasVariantType kind = DAS_VARIANT_TYPE_NULL;
            DasResult      result = input_map->GetType(key.Get(), &kind);
            if (DAS::IsFailed(result))
            {
                return result;
            }

            switch (kind)
            {
            case DAS_VARIANT_TYPE_INT:
            {
                int64_t value = 0;
                result = input_map->GetInt(key.Get(), &value);
                if (DAS::IsOk(result))
                {
                    result = output_map->SetInt(key.Get(), value);
                }
                return result;
            }
            case DAS_VARIANT_TYPE_FLOAT:
            {
                double value = 0.0;
                result = input_map->GetFloat(key.Get(), &value);
                if (DAS::IsOk(result))
                {
                    result = output_map->SetFloat(key.Get(), value);
                }
                return result;
            }
            case DAS_VARIANT_TYPE_BOOL:
            {
                bool value = false;
                result = input_map->GetBool(key.Get(), &value);
                if (DAS::IsOk(result))
                {
                    result = output_map->SetBool(key.Get(), value);
                }
                return result;
            }
            case DAS_VARIANT_TYPE_STRING:
            {
                DAS::DasPtr<IDasReadOnlyString> value;
                result = input_map->GetString(key.Get(), value.Put());
                if (DAS::IsOk(result))
                {
                    result = output_map->SetString(key.Get(), value.Get());
                }
                return result;
            }
            case DAS_VARIANT_TYPE_IMAGE:
            {
                DAS::DasPtr<Das::ExportInterface::IDasImage> value;
                result = input_map->GetImage(key.Get(), value.Put());
                if (DAS::IsOk(result))
                {
                    result = output_map->SetImage(key.Get(), value.Get());
                }
                return result;
            }
            case DAS_VARIANT_TYPE_BASE:
            {
                DAS::DasPtr<IDasBase> value;
                result =
                    input_map->GetBase(key.Get(), DAS_IID_BASE, value.Put());
                if (DAS::IsOk(result))
                {
                    result = output_map->SetBase(
                        key.Get(),
                        DAS_IID_BASE,
                        value.Get());
                }
                return result;
            }
            case DAS_VARIANT_TYPE_COMPONENT:
            {
                DAS::DasPtr<IDasBase> value;
                result = input_map->GetComponent(
                    key.Get(),
                    DAS_IID_BASE,
                    value.Put());
                if (DAS::IsOk(result))
                {
                    result = output_map->SetComponent(
                        key.Get(),
                        DAS_IID_BASE,
                        value.Get());
                }
                return result;
            }
            case DAS_VARIANT_TYPE_JSON:
            {
                DAS::DasPtr<IDasReadOnlyString> value;
                result = input_map->GetJson(key.Get(), value.Put());
                if (DAS::IsOk(result))
                {
                    result = output_map->SetString(key.Get(), value.Get());
                }
                return result;
            }
            case DAS_VARIANT_TYPE_NULL:
            case DAS_VARIANT_TYPE_SIGNAL:
            {
                return DAS_S_OK;
            }
            default:
            {
                return DAS_E_TYPE_ERROR;
            }
            }
        }
    } // namespace

    GraphTaskBoundaryComponent::GraphTaskBoundaryComponent(
        std::string_view                                       kind,
        DasPtr<Das::PluginInterface::IDasTaskComponentCreator> host)
        : kind_(kind), host_(std::move(host))
    {
    }

    DasResult GraphTaskBoundaryComponent::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        // 组件身份 = manifest 组件 GUID（单一事实源在 Core 的
        // GraphBoundaryComponents.h 常量）；类 GUID 仅供 ImplBase 模板
        // 实例化，不对外。
        const std::string component_guid_text{
            kind_ == GRAPH_BEGIN_KIND
                ? Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID
                : Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID};
        return DasMakeDasGuid(component_guid_text.c_str(), p_out_guid);
    }

    DasResult GraphTaskBoundaryComponent::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.Plugins.DasGraphTask.GraphTaskBoundaryComponent",
            pp_out_name);
    }

    DasResult GraphTaskBoundaryComponent::ApplySettingsChange(
        Das::ExportInterface::IDasJson*  p_request_json,
        Das::ExportInterface::IDasJson** pp_out_result_json)
    {
        std::ignore = p_request_json;
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;
        return DAS_S_OK;
    }

    // === GraphTaskBoundaryHostTask（capability 托管载体）===

    GraphTaskBoundaryHostTask::GraphTaskBoundaryHostTask(DasGuid component_guid)
        : component_guid_(component_guid)
    {
    }

    DasResult GraphTaskBoundaryHostTask::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        // 组件身份 = begin/end 边界组件 GUID（Core GraphBoundaryComponents.h
        // 单一事实源经构造传入）——capability 注册面按此 guid 建 record 并
        // 供 QI 链命中。
        *p_out_guid = component_guid_;
        return DAS_S_OK;
    }

    DasResult GraphTaskBoundaryHostTask::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.Plugins.DasGraphTask.GraphTaskBoundaryHostTask",
            pp_out_name);
    }

    DasResult GraphTaskBoundaryHostTask::Do(
        Das::PluginInterface::IDasStopToken* stop_token,
        Das::ExportInterface::IDasJson*      p_environment_json,
        Das::ExportInterface::IDasJson*      p_task_settings_json)
    {
        // 结构组件不可作为顶层任务执行——确定性拒绝（无副作用）。
        std::ignore = stop_token;
        std::ignore = p_environment_json;
        std::ignore = p_task_settings_json;
        return DAS_E_INVALID_ARGUMENT;
    }

    DasResult GraphTaskBoundaryHostTask::GetNextExecutionTime(
        Das::ExportInterface::IDasJson* p_task_settings_json,
        Das::ExportInterface::DasDate*  p_out_date)
    {
        // 结构组件不参与排程——无下次执行时间语义（既有「不排程」返回码
        // 形态，调度器按无排程处理）。
        std::ignore = p_task_settings_json;
        std::ignore = p_out_date;
        return DAS_E_NO_IMPLEMENTATION;
    }

    DasResult GraphTaskBoundaryComponent::Do(
        Das::PluginInterface::IDasStopToken*       stop_token,
        Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        Das::ExportInterface::IDasPortMap**        pp_out_port_map)
    {
        if (pp_out_port_map == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_port_map = nullptr;

        // 纯转写原语：无等待无子调用，不轮询取消。
        (void)stop_token;

        DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        if (p_input_port_map != nullptr)
        {
            DAS::DasPtr<Das::ExportInterface::IDasStringVector> keys;
            hr = p_input_port_map->GetKeys(keys.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            uint64_t key_count = 0;
            hr = keys->Size(&key_count);
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            for (uint64_t index = 0; index < key_count; ++index)
            {
                DAS::DasPtr<IDasReadOnlyString> key_string;
                if (DAS::IsFailed(keys->At(index, key_string.Put())))
                {
                    return DAS_E_FAIL;
                }
                if (key_string.Get() == nullptr)
                {
                    continue;
                }
                const char* key_utf8 = nullptr;
                if (DAS::IsFailed(key_string->GetUtf8(&key_utf8))
                    || key_utf8 == nullptr)
                {
                    continue;
                }
                hr = TransposePortEntry(
                    p_input_port_map,
                    output_map.Get(),
                    std::string(key_utf8));
                if (DAS::IsFailed(hr))
                {
                    // 转写失败=输入侧类型态损坏，显性失败防静默丢值。
                    return hr;
                }
            }
        }

        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

} // namespace Das::Plugins::DasGraphTask
