#pragma once

#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasPluginManager.Implements.hpp>

namespace Das::Http
{

    /**
     * @brief IDasPluginManager Stub。CreateComponent 委托到
     * ComponentFactoryManager，CreateCaptureManager 返回 NOT_IMPLEMENTED。
     */
    class DasPluginManagerServiceImpl final
        : public Das::ExportInterface::DasPluginManagerImplBase<
              DasPluginManagerServiceImpl>
    {
    public:
        explicit DasPluginManagerServiceImpl(
            Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
            Das::Core::SettingsManager::SettingsManager&    settings_manager);

        DAS_IMPL CreateComponent(
            const DasGuid&                        iid,
            Das::PluginInterface::IDasComponent** pp_out_component) override;

        DAS_IMPL CreateCaptureManager(
            IDasReadOnlyString*                        p_environment_config,
            Das::ExportInterface::IDasCaptureManager** pp_out_capture_manager)
            override;

    private:
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager_;
        Das::Core::SettingsManager::SettingsManager&    settings_manager_;
    };

} // namespace Das::Http
