#include <das/Plugins/GraphTask/GraphTaskImpl.h>

#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/Utils/CommonUtils.hpp>

// IID for GraphTaskImpl — required by IDasTypeInfo::GetGuid.
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::GraphTask,
    GraphTaskImpl,
    0xF4A2C9D1,
    0x7B3E,
    0x4D8A,
    0xA1,
    0x5F,
    0xC2,
    0xE8,
    0x6B,
    0x4D,
    0x91,
    0xA3)

namespace Das::Plugins::GraphTask
{

    GraphTaskImpl::GraphTaskImpl(
        Das::PluginInterface::IDasTaskComponentHost* p_host)
        : host_(p_host)
    {
    }

    DasResult GraphTaskImpl::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<GraphTaskImpl>();
        return DAS_S_OK;
    }

    DasResult GraphTaskImpl::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.Plugins.GraphTask.GraphTaskImpl",
            pp_out_name);
    }

    DasResult GraphTaskImpl::ApplySettingsChange(
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

    DasResult GraphTaskImpl::Do(
        Das::PluginInterface::IDasStopToken* stop_token,
        Das::ExportInterface::IDasJson*      p_environment_json,
        Das::ExportInterface::IDasJson*      p_settings_json,
        Das::ExportInterface::IDasJson*      p_input_json,
        Das::ExportInterface::IDasJson**     pp_out_result_json)
    {
        std::ignore = p_environment_json;
        std::ignore = p_input_json;
        std::ignore = p_settings_json;
        std::ignore = stop_token;
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

        // v18: GraphRuntime execution will be wired once the engine
        // is exposed through the DasCore DLL boundary.
        // Factory pattern and host injection are in place.
        return DAS_S_OK;
    }

} // namespace Das::Plugins::GraphTask
