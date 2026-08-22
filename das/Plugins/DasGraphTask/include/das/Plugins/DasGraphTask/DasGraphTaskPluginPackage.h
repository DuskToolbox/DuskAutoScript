#ifndef DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H
#define DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

namespace Das::Plugins::DasGraphTask
{

    // The task component host reaches the plugin through the component
    // factory's SetTaskComponentHost, which runs after every feature
    // interface is created — so the IDasTask adapter cannot capture the host
    // at construction. The factory publishes the host into this plugin-wide
    // slot and the adapter reads it at Do time. Non-owning: the factory's
    // DasPtr holds the reference and both feature interfaces are created and
    // released together with the loaded plugin.
    Das::PluginInterface::IDasTaskComponentHost* SharedTaskComponentHost();
    void                                         SetSharedTaskComponentHost(
        Das::PluginInterface::IDasTaskComponentHost* host);

    class DasGraphTaskComponentFactory final
        : public Das::PluginInterface::DasTaskComponentFactoryImplBase<
              DasGraphTaskComponentFactory>
    {
        DasPtr<Das::PluginInterface::IDasTaskComponentHost> host_;

    public:
        DasGraphTaskComponentFactory() = default;

        DAS_IMPL CreateComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component)
            override;

        DAS_IMPL SetTaskComponentHost(
            Das::PluginInterface::IDasTaskComponentHost* p_host) override;
    };

    class DasGraphTaskPluginPackage final
        : public Das::PluginInterface::DasPluginPackageImplBase<
              DasGraphTaskPluginPackage>
    {
    public:
        DasGraphTaskPluginPackage() = default;

        DAS_IMPL EnumFeature(
            size_t                                  index,
            Das::PluginInterface::DasPluginFeature* p_out_feature) override;

        DAS_IMPL CreateFeatureInterface(
            size_t     index,
            IDasBase** pp_out_interface) override;
    };

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H
