#pragma once

#include <das/IDasPluginManagerService.h>
#include <das/IDasSettingsService.h>
#include <das/_autogen/idl/abi/DasSettings.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProfileService.Implements.hpp>
#include <string>

namespace Das::Http
{

    /**
     * @brief IDasProfileService Stub。GetProfile(guid) 查询 PluginManager
     * 获取白名单后创建 IDasPluginProfile。
     */
    class DasProfileServiceImpl final
        : public Das::ExportInterface::DasProfileServiceImplBase<
              DasProfileServiceImpl>
    {
    public:
        explicit DasProfileServiceImpl(
            IDasPluginManagerService& plugin_manager_service,
            IDasSettingsService&      settings_service);

        DAS_IMPL GetProfile(
            const DasGuid&                            plugin_guid,
            Das::ExportInterface::IDasPluginProfile** pp_out) override;

    private:
        IDasPluginManagerService& plugin_manager_service_;
        IDasSettingsService&      settings_service_;
    };

} // namespace Das::Http
