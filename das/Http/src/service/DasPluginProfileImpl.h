#pragma once

#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/_autogen/idl/abi/DasSettings.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasPluginProfile.Implements.hpp>
#include <string>
#include <unordered_set>

namespace Das::Http
{

    class DasAutoFlushJsonImpl;

    // IDasPluginProfile Stub。GetSettingJson() 返回缓存的 DasAutoFlushJsonImpl
    // 实例。
    class DasPluginProfileImpl final
        : public Das::ExportInterface::DasPluginProfileImplBase<
              DasPluginProfileImpl>
    {
    public:
        DasPluginProfileImpl(
            Das::Core::SettingsManager::SettingsManager& settings_manager,
            std::string                                  profile_id,
            std::string                                  plugin_guid,
            std::unordered_set<std::string>              whitelist);

        DAS_IMPL GetSettingJson(
            Das::ExportInterface::IDasJson** pp_out) override;

    private:
        Das::Core::SettingsManager::SettingsManager& settings_manager_;
        std::string                                  profile_id_;
        std::string                                  plugin_guid_;
        std::unordered_set<std::string>              whitelist_;

        DasPtr<DasAutoFlushJsonImpl> cached_json_;
    };

} // namespace Das::Http
