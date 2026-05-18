#include "DasPluginProfileImpl.h"

#include "DasAutoFlushJsonImpl.h"

#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>

namespace Das::Http
{

    DasPluginProfileImpl::DasPluginProfileImpl(
        IDasSettingsService&            settings_service,
        std::string                     profile_id,
        std::string                     plugin_guid,
        std::unordered_set<std::string> whitelist)
        : settings_service_{settings_service},
          profile_id_{std::move(profile_id)},
          plugin_guid_{std::move(plugin_guid)}, whitelist_{std::move(whitelist)}
    {
    }

    DasResult DasPluginProfileImpl::GetSettingJson(
        Das::ExportInterface::IDasJson** pp_out)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out)

        if (!cached_json_)
        {
            try
            {
                cached_json_ = new DasAutoFlushJsonImpl(
                    settings_service_,
                    profile_id_,
                    plugin_guid_,
                    whitelist_);
            }
            catch (const std::bad_alloc& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return DAS_E_OUT_OF_MEMORY;
            }
        }

        *pp_out = cached_json_.Get();
        (*pp_out)->AddRef();
        return DAS_S_OK;
    }

} // namespace Das::Http
