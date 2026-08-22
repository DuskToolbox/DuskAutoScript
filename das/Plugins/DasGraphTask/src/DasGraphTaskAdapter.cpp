#define DAS_BUILD_SHARED

#include <das/Plugins/DasGraphTask/DasGraphTaskAdapter.h>

#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Plugins/DasGraphTask/DasGraphTaskPluginPackage.h>

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    DasResult DasGraphTaskAdapter::Do(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*      p_environment_json,
        ExportInterface::IDasJson*      p_task_settings_json)
    {
        std::ignore = p_environment_json;

        // task_settings is the CompiledGraphPlanDto serialization the
        // scheduler extracted from the authoring compile envelope's
        // compile.executionInput. GraphRuntime::Execute casts its input to
        // the very same DTO, so this is a pure pass-through — no field
        // extraction or transformation.
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

        // Node components are created through the task component host the
        // plugin's component factory received at load time (shared
        // plugin-wide, bound after this adapter is constructed). Without a
        // host the runtime can only reject the graph at Configure, so fall
        // back to the hostless factory only when no host was ever bound.
        auto* component_host = SharedTaskComponentHost();
        DAS::DasPtr<Das::ExportInterface::IDasGraphRuntime> runtime;
        const DasResult                                     hr =
            component_host != nullptr
                ? CreateGraphRuntimeWithHost(component_host, runtime.Put())
                : CreateGraphRuntime(runtime.Put());
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
