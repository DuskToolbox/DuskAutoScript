#pragma once

#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/_autogen/idl/abi/DasSettings.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProfileService.Implements.hpp>
#include <string>

namespace Das::Http
{

    /**
     * @brief IDasProfileService Stub 实现
     *
     * GetProfile(guid) 解析插件 GUID 到 IDasPluginProfile。
     * 从 PluginManager 查询 GUID 所属 Package 的 settings_desc
     * 构建白名单（D-12/D-14）。 生命周期由 DistributedObjectManager
     * 管理（D-16）。
     */
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
