#ifndef DAS_PLUGINS_GRAPHTASK_GRAPHTASKIMPL_H
#define DAS_PLUGINS_GRAPHTASK_GRAPHTASKIMPL_H

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>

#include <string>

namespace Das::Plugins::GraphTask
{

    // Thin adapter: IDasTaskComponent -> GraphRuntime execution.
    // Receives compiled graph plan JSON via settings, creates
    // GraphRuntime internally, and runs via RunWithHost().
    class GraphTaskImpl final
        : public Das::PluginInterface::DasTaskComponentImplBase<GraphTaskImpl>
    {
        std::string                                         last_error_;
        DasPtr<Das::PluginInterface::IDasTaskComponentHost> host_;

    public:
        explicit GraphTaskImpl(
            Das::PluginInterface::IDasTaskComponentHost* p_host);

        // --- IDasTaskComponent interface ---
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL ApplySettingsChange(
            Das::ExportInterface::IDasJson*  p_request_json,
            Das::ExportInterface::IDasJson** pp_out_result_json) override;

        DAS_IMPL Do(
            Das::PluginInterface::IDasStopToken* stop_token,
            Das::ExportInterface::IDasJson*      p_environment_json,
            Das::ExportInterface::IDasJson*      p_settings_json,
            Das::ExportInterface::IDasJson*      p_input_json,
            Das::ExportInterface::IDasJson**     pp_out_result_json) override;
    };

} // namespace Das::Plugins::GraphTask

#endif // DAS_PLUGINS_GRAPHTASK_GRAPHTASKIMPL_H
