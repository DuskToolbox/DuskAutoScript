#ifndef DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H
#define DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

namespace Das::Plugins::DasGraphTask
{

    class DasGraphTaskPluginPackage final
        : public Das::PluginInterface::IDasPluginPackage,
          public Das::PluginInterface::DasTaskComponentFactoryImplBase<
              DasGraphTaskPluginPackage>
    {
        DasPtr<Das::PluginInterface::IDasTaskComponentHost> host_;

    public:
        DasGraphTaskPluginPackage() = default;

        // --- IUnknown (override to extend base QI with IDasPluginPackage) ---
        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override;

        // --- IDasPluginPackage ---
        DAS_IMPL EnumFeature(
            size_t                                  index,
            Das::PluginInterface::DasPluginFeature* p_out_feature) override;

        DAS_IMPL CreateFeatureInterface(
            size_t     index,
            IDasBase** pp_out_interface) override;

        DAS_IMPL CanUnloadNow(bool* p_can_unload) override;

        // --- IDasTaskComponentFactory ---
        DAS_IMPL CreateComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component)
            override;

        DAS_IMPL SetTaskComponentHost(
            Das::PluginInterface::IDasTaskComponentHost* p_host) override;
    };

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H
