#include "DasPluginManagerServiceImpl.h"

#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>

namespace Das::Http
{

    DasResult DasPluginManagerServiceImpl::CreateComponent(
        const DasGuid&                        iid,
        Das::PluginInterface::IDasComponent** pp_out_component)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_component)

        auto& factory_mgr =
            Das::Core::ForeignInterfaceHost::PluginManager::GetInstance()
                .GetComponentFactoryManager();

        return factory_mgr.CreateComponent(iid, pp_out_component);
    }

    DasResult DasPluginManagerServiceImpl::CreateCaptureManager(
        IDasReadOnlyString*                        p_environment_config,
        Das::ExportInterface::IDasCaptureManager** pp_out_capture_manager)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_capture_manager)
        return DAS_E_NO_IMPLEMENTATION;
    }

} // namespace Das::Http
