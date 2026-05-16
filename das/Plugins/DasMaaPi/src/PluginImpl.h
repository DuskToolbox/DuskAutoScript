#ifndef DAS_PLUGINS_DASMAAPI_PLUGINIMPL_H
#define DAS_PLUGINS_DASMAAPI_PLUGINIMPL_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    class DasMaaPiPlugin final
        : public PluginInterface::DasPluginPackageImplBase<DasMaaPiPlugin>
    {
    public:
        DAS_IMPL EnumFeature(
            size_t                             index,
            PluginInterface::DasPluginFeature* p_out_feature) override;

        DAS_IMPL CreateFeatureInterface(
            size_t     index,
            IDasBase** pp_out_interface) override;

        DAS_IMPL CanUnloadNow(bool* p_can_unload) override;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_PLUGINIMPL_H
