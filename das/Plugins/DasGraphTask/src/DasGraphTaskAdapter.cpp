#define DAS_BUILD_SHARED

#include <das/Plugins/DasGraphTask/DasGraphTaskAdapter.h>

#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    DasResult DasGraphTaskAdapter::Do(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*      p_environment_json,
        ExportInterface::IDasJson*      p_task_settings_json)
    {
        std::ignore = p_environment_json;

        // task_settings is the CompiledGraphPlanDto serialization produced by
        // the scheduler's authoring Compile (ExtractExecutionInput returns the
        // whole DTO since it has no "executionInput" wrapper). GraphRuntime::
        // Execute casts its input to the very same DTO, so this is a pure
        // pass-through — no field extraction or transformation.
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

        // IDasTask contract: do not implicitly access host/storage; execute the
        // self-contained snapshot via the hostless factory (matches the
        // original 78935d8e IDasTask form).
        DAS::DasPtr<Das::ExportInterface::IDasGraphRuntime> runtime;
        DasResult hr = CreateGraphRuntime(runtime.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        // IDasTask::Do has no structured return value (fire-and-forget, same as
        // MaapiTask); the result JSON is discarded.
        DAS::DasPtr<Das::ExportInterface::IDasJson> result;
        return runtime->Execute(artifact.Get(), stop_token, result.Put());
    }

    DasResult DasGraphTaskAdapter::GetNextExecutionTime(
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
