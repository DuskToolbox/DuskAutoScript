#include "DasPluginManagerServiceImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>

namespace Das::Http
{

    DasPluginManagerServiceImpl::DasPluginManagerServiceImpl(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        Das::Core::SettingsManager::SettingsManager&    settings_manager)
        : plugin_manager_(plugin_manager), settings_manager_(settings_manager)
    {
    }

    DasResult DasPluginManagerServiceImpl::CreateComponent(
        const DasGuid&                        iid,
        Das::PluginInterface::IDasComponent** pp_out_component)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_component)

        auto& factory_mgr = plugin_manager_.GetComponentFactoryManager();

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
