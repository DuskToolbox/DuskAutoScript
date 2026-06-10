#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>

#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>

namespace Das::Plugins::DasGraphTask
{

    DasGraphTaskImpl::DasGraphTaskImpl(
        Das::PluginInterface::IDasTaskComponentHost* p_host)
        : host_(p_host)
    {
    }

    DasResult DasGraphTaskImpl::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<DasGraphTaskImpl>();
        return DAS_S_OK;
    }

    DasResult DasGraphTaskImpl::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.Plugins.DasGraphTask.DasGraphTaskImpl",
            pp_out_name);
    }

    DasResult DasGraphTaskImpl::ApplySettingsChange(
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

    DasResult DasGraphTaskImpl::Do(
        Das::PluginInterface::IDasStopToken*       stop_token,
        Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        Das::ExportInterface::IDasPortMap**        pp_out_port_map)
    {
        if (pp_out_port_map == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_port_map = nullptr;

        if (!host_)
        {
            last_error_ = "Task component host is unavailable";
            DAS_LOG_ERROR(last_error_.c_str());
            return DAS_E_INVALID_POINTER;
        }

        // Read compiled artifact string from "compiledPlan" port
        DAS::DasPtr<IDasReadOnlyString> p_artifact;
        DasResult                       hr = DAS_S_OK;
        if (p_input_port_map != nullptr)
        {
            DasReadOnlyString compiled_plan_key{"compiledPlan"};
            hr = p_input_port_map->GetString(
                compiled_plan_key.Get(),
                p_artifact.Put());
            // It's OK if the port doesn't exist — we pass null artifact.
        }

        // Create GraphRuntime with host bound at construction.
        DAS::DasPtr<Das::ExportInterface::IDasGraphRuntime> runtime;
        hr = CreateGraphRuntimeWithHost(host_.Get(), runtime.Put());
        if (DAS::IsFailed(hr))
        {
            last_error_ = "Failed to create GraphRuntime";
            DAS_LOG_ERROR(last_error_.c_str());
            return hr;
        }

        // Execute compiled artifact via GraphRuntime
        DAS::DasPtr<Das::ExportInterface::IDasJson> result_json;
        hr = runtime->Execute(p_artifact.Get(), stop_token, result_json.Put());

        if (DAS::IsFailed(hr))
        {
            DAS::DasPtr<IDasReadOnlyString> p_error;
            if (DAS::IsOk(runtime->GetErrorMessage(p_error.Put())) && p_error)
            {
                const char* utf8 = nullptr;
                p_error->GetUtf8(&utf8);
                if (utf8 && *utf8)
                {
                    last_error_ = utf8;
                }
            }
            if (last_error_.empty())
            {
                last_error_ = "Graph execution failed";
            }
            DAS_LOG_ERROR(last_error_.c_str());
            return hr;
        }

        // Serialize result JSON → string, put into output PortMap under
        // "result" port.
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
        hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        if (result_json.Get())
        {
            DAS::DasPtr<IDasReadOnlyString> p_result_str;
            hr = result_json->ToString(0, p_result_str.Put());
            if (DAS::IsOk(hr) && p_result_str.Get())
            {
                DasReadOnlyString result_key{"result"};
                output_map->SetString(result_key.Get(), p_result_str.Get());
            }
        }

        *pp_out_port_map = output_map.Get();
        if (output_map.Get())
        {
            output_map.Get()->AddRef();
        }
        return DAS_S_OK;
    }

} // namespace Das::Plugins::DasGraphTask
