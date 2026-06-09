#include <das/Plugins/GraphTask/GraphTaskImpl.h>

#include <das/DasApi.h>
#include <das/DasGuidHolder.h>

// IID for GraphTaskImpl — required by IDasTypeInfo::GetGuid via
// DasTaskImplBase<T>.
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

    DasResult GraphTaskImpl::Do(
        Das::PluginInterface::IDasStopToken* p_stop_token,
        Das::ExportInterface::IDasJson*      p_environment_json,
        Das::ExportInterface::IDasJson*      p_task_settings_json)
    {
        std::ignore = p_stop_token;
        std::ignore = p_environment_json;
        std::ignore = p_task_settings_json;

        // GraphRuntime::Load() and Run() have been removed from the IDL;
        // graph execution now goes through GraphRuntime::RunWithHost()
        // directly in C++ code, not through the COM facade.
        last_error_ = "GraphTask execution is not available via COM interface";
        DAS_LOG_ERROR(last_error_.c_str());
        return DAS_E_NO_IMPLEMENTATION;
    }

    DasResult GraphTaskImpl::GetNextExecutionTime(
        Das::ExportInterface::DasDate* p_out_date)
    {
        if (p_out_date)
        {
            *p_out_date = {};
        }
        return DAS_S_FALSE;
    }

} // namespace Das::Plugins::GraphTask
