#pragma once

#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasPluginManager.Implements.hpp>

namespace Das::Http
{

    /**
     * @brief IDasPluginManager Stub。CreateComponent 委托到
     * ComponentFactoryManager，CreateCaptureManager 返回 NOT_IMPLEMENTED。
     */
    class DasPluginManagerServiceImpl final
        : public Das::ExportInterface::DasPluginManagerImplBase<
              DasPluginManagerServiceImpl>
    {
    public:
        DasPluginManagerServiceImpl() = default;

        DAS_IMPL CreateComponent(
            const DasGuid&                        iid,
            Das::PluginInterface::IDasComponent** pp_out_component) override;

        DAS_IMPL CreateCaptureManager(
            IDasReadOnlyString*                        p_environment_config,
            Das::ExportInterface::IDasCaptureManager** pp_out_capture_manager)
            override;
    };

} // namespace Das::Http
