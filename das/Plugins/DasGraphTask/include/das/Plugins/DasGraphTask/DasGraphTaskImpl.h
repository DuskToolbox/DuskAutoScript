#ifndef DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKIMPL_H
#define DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKIMPL_H

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>

#include <string>

namespace Das::Plugins::DasGraphTask
{

    // Thin adapter: IDasTaskComponent -> GraphRuntime execution.
    // Delegates to IDasGraphRuntime::Execute() via the public DasCore API.
    class DasGraphTaskImpl final
        : public Das::PluginInterface::DasTaskComponentImplBase<
              DasGraphTaskImpl>
    {
        std::string                                         last_error_;
        DasPtr<Das::PluginInterface::IDasTaskComponentHost> host_;

    public:
        explicit DasGraphTaskImpl(
            Das::PluginInterface::IDasTaskComponentHost* p_host);

        // --- IDasTaskComponent interface ---
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

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

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKIMPL_H
