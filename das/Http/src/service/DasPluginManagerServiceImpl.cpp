#include "DasPluginManagerServiceImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>

namespace Das::Http
{

    DasPluginManagerServiceImpl::DasPluginManagerServiceImpl(
        IDasPluginManagerService& plugin_manager_service,
        IDasSettingsService&      settings_service)
        : plugin_manager_service_(plugin_manager_service),
          settings_service_(settings_service)
    {
    }

    DasResult DasPluginManagerServiceImpl::CreateComponent(
        const DasGuid&                        iid,
        Das::PluginInterface::IDasComponent** pp_out_component)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_component)

        return plugin_manager_service_.CreateComponent(
            iid,
            reinterpret_cast<void**>(pp_out_component));
    }

    DasResult DasPluginManagerServiceImpl::CreateCaptureManager(
        IDasReadOnlyString*                        p_environment_config,
        Das::ExportInterface::IDasCaptureManager** pp_out_capture_manager)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_capture_manager)
        DAS_UTILS_CHECK_POINTER(p_environment_config)

        return plugin_manager_service_.CreateCaptureManager(
            p_environment_config,
            &settings_service_,
            reinterpret_cast<void**>(pp_out_capture_manager));
    }

} // namespace Das::Http
