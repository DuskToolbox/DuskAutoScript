#ifndef DAS_PLUGINS_DASMAAPI_MAAPIAGENTCOMPONENT_H
#define DAS_PLUGINS_DASMAAPI_MAAPIAGENTCOMPONENT_H

#include "AgentProcessRunner.h"
#include "AgentRuntimeService.h"

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasComponent.Implements.hpp>

#include <memory>
#include <string>
#include <string_view>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiAgentComponent,
    0x69f20004,
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
    class MaapiAgentComponent final
        : public PluginInterface::DasComponentImplBase<MaapiAgentComponent>
    {
    public:
        MaapiAgentComponent();
        MaapiAgentComponent(
            AgentRuntime::AgentRuntimeService&   service,
            AgentRuntime::AgentRuntimeMaaContext context);
        ~MaapiAgentComponent() override;

        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL Dispatch(
            IDasReadOnlyString*                  p_function_name,
            ExportInterface::IDasVariantVector*  p_arguments,
            ExportInterface::IDasVariantVector** pp_out_result) override;

    private:
        std::unique_ptr<AgentRuntime::BoostAgentProcessRunner> owned_runner_;
        std::unique_ptr<AgentRuntime::AgentRuntimeService>     owned_service_;
        AgentRuntime::AgentRuntimeService*   service_ = nullptr;
        AgentRuntime::AgentRuntimeMaaContext context_{};
    };

    DasResult DispatchMaapiAgentComponentJson(
        PluginInterface::IDasComponent& component,
        std::string_view                function_name,
        std::string_view                request_json,
        std::string&                    out_result_json);
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIAGENTCOMPONENT_H
