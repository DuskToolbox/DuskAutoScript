#ifndef DAS_PLUGINS_GRAPHTASK_GRAPHTASKIMPL_H
#define DAS_PLUGINS_GRAPHTASK_GRAPHTASKIMPL_H

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTask.Implements.hpp>

#include <string>

namespace Das::Plugins::GraphTask
{

    // Thin adapter: IDasTask -> GraphRuntime execution.
    // Per CORE-FIRST-v18: zero business logic — pure delegation.
    class GraphTaskImpl final
        : public Das::PluginInterface::DasTaskImplBase<GraphTaskImpl>
    {
        std::string last_error_;

    public:
        GraphTaskImpl() = default;

        // --- IDasTask interface ---
        DAS_IMPL Do(
            Das::PluginInterface::IDasStopToken* p_stop_token,
            Das::ExportInterface::IDasJson*      p_environment_json,
            Das::ExportInterface::IDasJson*      p_task_settings_json);

        DAS_IMPL GetNextExecutionTime(
            Das::ExportInterface::DasDate* p_out_date);
    };

} // namespace Das::Plugins::GraphTask

#endif // DAS_PLUGINS_GRAPHTASK_GRAPHTASKIMPL_H
