#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>

#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

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
        Das::PluginInterface::IDasStopToken* stop_token,
        Das::ExportInterface::IDasJson*      p_environment_json,
        Das::ExportInterface::IDasJson*      p_settings_json,
        Das::ExportInterface::IDasJson*      p_input_json,
        Das::ExportInterface::IDasJson**     pp_out_result_json)
    {
        std::ignore = p_environment_json;
        std::ignore = p_input_json;

        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        if (!host_)
        {
            last_error_ = "Task component host is unavailable";
            DAS_LOG_ERROR(last_error_.c_str());
            return DAS_E_INVALID_POINTER;
        }

        // Serialize settings JSON to string for the Execute() call.
        DAS::DasPtr<IDasReadOnlyString> p_settings_str;
        if (p_settings_json != nullptr)
        {
            DasResult hr = p_settings_json->ToString(0, p_settings_str.Put());
            if (DAS::IsFailed(hr) || !p_settings_str.Get())
            {
                last_error_ = "Failed to serialize settings JSON";
                DAS_LOG_ERROR(last_error_.c_str());
                return DAS_E_INVALID_POINTER;
            }
        }

        // Create GraphRuntime with host bound at construction.
        DAS::DasPtr<Das::ExportInterface::IDasGraphRuntime> runtime;
        DasResult hr = CreateGraphRuntimeWithHost(host_.Get(), runtime.Put());
        if (DAS::IsFailed(hr))
        {
            last_error_ = "Failed to create GraphRuntime";
            DAS_LOG_ERROR(last_error_.c_str());
            return hr;
        }

        DAS::DasPtr<Das::ExportInterface::IDasJson> result_json;
        hr = runtime->Execute(
            p_settings_str.Get(),
            stop_token,
            result_json.Put());

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

        *pp_out_result_json = result_json.Get();
        if (result_json.Get())
        {
            result_json.Get()->AddRef();
        }
        return DAS_S_OK;
    }

} // namespace Das::Plugins::DasGraphTask
