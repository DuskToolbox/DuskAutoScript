#pragma once

#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/_autogen/idl/abi/DasSettings.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProfileService.Implements.hpp>
#include <string>

namespace Das::Http
{

    // IDasProfileService Stub。GetProfile(guid) 查询 PluginManager
    // 获取白名单后创建 IDasPluginProfile。
    class DasProfileServiceImpl final
        : public Das::ExportInterface::DasProfileServiceImplBase<
              DasProfileServiceImpl>
    {
    public:
        explicit DasProfileServiceImpl(
            Das::Core::SettingsManager::SettingsManager& settings_manager);

        DAS_IMPL GetProfile(
            const DasGuid&                            plugin_guid,
            Das::ExportInterface::IDasPluginProfile** pp_out) override;

    private:
        Das::Core::SettingsManager::SettingsManager& settings_manager_;
    };

} // namespace Das::Http
