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

        // Load the compiled graph artifact from task settings JSON.
        if (p_task_settings_json)
        {
            Das::DasPtr<IDasReadOnlyString> artifact_str;
            hr = p_task_settings_json->ToString(0, artifact_str.Put());
            if (DAS::IsFailed(hr) || !artifact_str.Get())
            {
                last_error_ = "Failed to serialize task settings JSON";
                return DAS::IsFailed(hr) ? hr : DAS_E_FAIL;
            }
            hr = runtime->Load(artifact_str.Get());
            if (DAS::IsFailed(hr))
            {
                last_error_ = "Failed to load compiled graph artifact";
                return hr;
            }
        }
        else
        {
            last_error_ =
                "No task settings provided — cannot load compiled graph artifact";
            DAS_LOG_ERROR(last_error_.c_str());
            return DAS_E_INVALID_POINTER;
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
        // Scheduling is not applicable for one-shot graph tasks.
        // Callers should treat this as "no future execution scheduled".
        return DAS_S_FALSE;
    }

} // namespace Das::Plugins::GraphTask
