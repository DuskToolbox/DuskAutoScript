#pragma once

#include <das/Core/ForeignInterfaceHost/IDasCaptureManagerImpl.h>
#include <das/DasString.hpp>
#include <das/IDasPluginManagerService.h>
#include <das/_autogen/idl/abi/IDasCapture.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasPluginManager.Implements.hpp>
#include <das/_autogen/idl/wrapper/IDasTypeInfo.hpp>

namespace Das::Http
{

    class DasPluginManagerServiceImpl final
        : public Das::ExportInterface::DasPluginManagerImplBase<
              DasPluginManagerServiceImpl>
    {
    public:
        explicit DasPluginManagerServiceImpl(
            IDasPluginManagerService& plugin_manager_service);

        DAS_IMPL CreateComponent(
            const DasGuid&                        iid,
            Das::PluginInterface::IDasComponent** pp_out_component) override;

        DAS_IMPL CreateCaptureManager(
            IDasReadOnlyString*                        p_environment_config,
            Das::ExportInterface::IDasCaptureManager** pp_out_capture_manager)
            override;

    private:
        IDasPluginManagerService& plugin_manager_service_;
    };

} // namespace Das::Http
