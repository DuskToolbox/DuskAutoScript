#include "DasPluginManagerServiceImpl.h"

#include <das/DasPtr.hpp>
#include <das/Utils/CommonUtils.hpp>

namespace Das::Http
{

    DasPluginManagerServiceImpl::DasPluginManagerServiceImpl(
        IDasPluginManagerService& plugin_manager_service)
        : plugin_manager_service_(plugin_manager_service)
    {
    }

    DasResult DasPluginManagerServiceImpl::CreateComponent(
        const DasGuid&                        iid,
        Das::PluginInterface::IDasComponent** pp_out_component)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_component)

        DAS::DasPtr<IDasBase> base_component;
        auto                  result =
            plugin_manager_service_.CreateComponent(&iid, base_component.Put());
        if (DAS::IsFailed(result))
        {
            return result;
        }
        if (!base_component)
        {
            return DAS_E_INVALID_POINTER;
        }

        return base_component->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasComponent>(),
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
            pp_out_capture_manager);
    }

} // namespace Das::Http
