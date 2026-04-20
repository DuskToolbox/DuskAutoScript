#include "DasProfileServiceImpl.h"

#include "DasPluginProfileImpl.h"

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

namespace Das::Http
{

    DasProfileServiceImpl::DasProfileServiceImpl(
        IDasPluginManagerService& plugin_manager_service,
        IDasSettingsService&      settings_service)
        : plugin_manager_service_(plugin_manager_service),
          settings_service_(settings_service)
    {
    }

    DasResult DasProfileServiceImpl::GetProfile(
        const DasGuid&                            plugin_guid,
        Das::ExportInterface::IDasPluginProfile** pp_out)
    {
        DAS_UTILS_CHECK_POINTER(pp_out)

        auto* desc =
            plugin_manager_service_.FindPluginPackageByGuid(plugin_guid);
        if (!desc)
        {
            DAS_CORE_LOG_ERROR("Plugin not found for GUID.");
            return DAS_E_NOT_FOUND;
        }

        std::unordered_set<std::string> whitelist;
        for (const auto& setting : desc->settings_desc)
        {
            whitelist.insert(setting.name);
        }

        const auto guid_str =
            Das::Core::ForeignInterfaceHost::DasGuidToStdString(plugin_guid);

        try
        {
            auto* profile_impl = new DasPluginProfileImpl(
                settings_service_,
                "0", // v1.2 hardcode: profile_id = "0"
                guid_str,
                std::move(whitelist));
            profile_impl->AddRef();
            *pp_out = profile_impl;
            return DAS_S_OK;
        }
        catch (const std::bad_alloc& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_OUT_OF_MEMORY;
        }
    }

} // namespace Das::Http
