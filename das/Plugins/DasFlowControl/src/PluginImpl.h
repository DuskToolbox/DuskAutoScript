#ifndef DAS_PLUGINS_DASFLOWCONTROL_PLUGINIMPL_H
#define DAS_PLUGINS_DASFLOWCONTROL_PLUGINIMPL_H

#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>

DAS_NS_BEGIN

class DasFlowControlPlugin final
    : public PluginInterface::DasPluginPackageImplBase<DasFlowControlPlugin>
{
public:
    DAS_IMPL EnumFeature(
        size_t                             index,
        PluginInterface::DasPluginFeature* p_out_feature) override;

    DAS_IMPL CreateFeatureInterface(size_t index, IDasBase** pp_out_interface)
        override;

    DAS_IMPL CanUnloadNow(bool* p_can_unload) override;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASFLOWCONTROL_PLUGINIMPL_H
