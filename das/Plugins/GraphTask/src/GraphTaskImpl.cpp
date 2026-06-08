#include <das/Plugins/GraphTask/GraphTaskImpl.h>

#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/fmt.h>

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
        std::ignore = p_environment_json;
        std::ignore = p_task_settings_json;

        // Check cancellation before doing any work.
        if (p_stop_token)
        {
            bool      can_stop = false;
            DasResult hr = p_stop_token->StopRequested(&can_stop);
            if (DAS::IsOk(hr) && can_stop)
            {
                last_error_ = "Execution cancelled before start";
                return DAS_E_FAIL;
            }
        }

        // Create GraphRuntime via C API factory (core-first v18: plugin only
        // delegates, no business logic).
        Das::DasPtr<Das::ExportInterface::IDasGraphRuntime> runtime;
        DasResult hr = CreateGraphRuntime(runtime.Put());
        if (DAS::IsFailed(hr))
        {
            last_error_ = "Failed to create GraphRuntime";
            return hr;
        }

        // Run the graph engine with the stop token.
        hr = runtime->Run(p_stop_token);
        if (DAS::IsFailed(hr))
        {
            Das::DasPtr<IDasReadOnlyString> error_msg;
            runtime->GetErrorMessage(error_msg.Put());
            if (error_msg.Get())
            {
                const char* str = nullptr;
                error_msg->GetUtf8(&str);
                if (str)
                {
                    last_error_ = str;
                }
            }
        }
        return hr;
    }

    DasResult GraphTaskImpl::GetNextExecutionTime(
        Das::ExportInterface::DasDate* p_out_date)
    {
        if (p_out_date)
        {
            *p_out_date = {};
        }
        return DAS_E_NO_IMPLEMENTATION;
    }

} // namespace Das::Plugins::GraphTask
