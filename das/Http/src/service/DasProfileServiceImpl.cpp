#include "DasProfileServiceImpl.h"

#include "DasPluginProfileImpl.h"

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

namespace Das::Http
{

    DasProfileServiceImpl::DasProfileServiceImpl(
        Das::Core::SettingsManager::SettingsManager& settings_manager)
        : settings_manager_{settings_manager}
    {
    }

    DasResult DasProfileServiceImpl::GetProfile(
        const DasGuid&                            plugin_guid,
        Das::ExportInterface::IDasPluginProfile** pp_out)
    {
        DAS_UTILS_CHECK_POINTER(pp_out)

        auto* desc =
            Das::Core::ForeignInterfaceHost::PluginManager::GetInstance()
                .FindPluginPackageByGuid(plugin_guid);
        if (!desc)
        {
            DAS_CORE_LOG_ERROR("Plugin not found for GUID.");
            return DAS_E_NOT_FOUND;
        }

        // Build whitelist from settings_desc (D-12/D-14)
        std::unordered_set<std::string> whitelist;
        for (const auto& setting : desc->settings_desc)
        {
            whitelist.insert(setting.name);
        }

        // Convert GUID to string for SettingsManager file lookup
        // DasReadOnlyString ctor calls DasGuidToString internally
        DasReadOnlyString guid_string(&plugin_guid);
        std::string       guid_str = guid_string.GetUtf8();

        // Strip surrounding braces if present
        if (guid_str.size() >= 2 && guid_str.front() == '{'
            && guid_str.back() == '}')
        {
            guid_str = guid_str.substr(1, guid_str.size() - 2);
        }

        try
        {
            auto* profile_impl = new DasPluginProfileImpl(
                settings_manager_,
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
