#ifndef DAS_PLUGINS_DASMAAPI_MAAPIAGENTTASKCOMPONENT_H
#define DAS_PLUGINS_DASMAAPI_MAAPIAGENTTASKCOMPONENT_H

#include "AgentProcessRunner.h"
#include "AgentRuntimeService.h"

#include <cpp_yyjson.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>

#include <memory>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiAgentTaskComponent,
    0x69f20006,
    0x0000,
    0x4000,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    class MaapiAgentTaskComponent final
        : public PluginInterface::DasTaskComponentImplBase<
              MaapiAgentTaskComponent>
    {
    public:
        MaapiAgentTaskComponent();
        MaapiAgentTaskComponent(
            AgentRuntime::AgentRuntimeService&      service,
            AgentRuntime::AgentRuntimeMaaContext    context);
        ~MaapiAgentTaskComponent() override;

        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL ApplySettingsChange(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;

        DAS_IMPL Do(
            PluginInterface::IDasStopToken* stop_token,
            ExportInterface::IDasJson*      p_environment_json,
            ExportInterface::IDasJson*      p_settings_json,
            ExportInterface::IDasJson*      p_input_json,
            ExportInterface::IDasJson**     pp_out_result_json) override;

    private:
        std::unique_ptr<AgentRuntime::BoostAgentProcessRunner> owned_runner_;
        std::unique_ptr<AgentRuntime::AgentRuntimeService>     owned_service_;
        AgentRuntime::AgentRuntimeService*                     service_ =
            nullptr;
        AgentRuntime::AgentRuntimeMaaContext context_{};
        yyjson::value settings_;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIAGENTTASKCOMPONENT_H
