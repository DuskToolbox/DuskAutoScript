#include "MaapiTask.h"
#include "PluginUtils.h"

#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/PiParser.h>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    DasResult MaapiTask::Do(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson* p_task_settings_json)
    {
        if (!p_task_settings_json)
        {
            return DAS_E_INVALID_POINTER;
        }

        auto request = ReadJson(p_task_settings_json);
        if (!request || !request->is_object())
        {
            return DAS_E_INVALID_JSON;
        }
        auto parsed = ParseExecutionEnvelope(*request);
        if (DAS::IsFailed(parsed.result))
        {
            return parsed.result;
        }

        auto result = MaaRuntime::Run(
            parsed.envelope,
            MaaApiBoundaryForRuntime(),
            stop_token);
        return result.das_result;
    }

    DasResult MaapiTask::GetNextExecutionTime(
        ExportInterface::DasDate* p_out_date)
    {
        if (!p_out_date)
        {
            return DAS_E_INVALID_POINTER;
        }
        return DAS_E_NO_IMPLEMENTATION;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
