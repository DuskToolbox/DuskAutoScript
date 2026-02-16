#ifndef DAS_PLUGINS_IPCTESTPLUGIN_PLUGINIMPL_H
#define DAS_PLUGINS_IPCTESTPLUGIN_PLUGINIMPL_H

#include <DAS/_autogen/idl/abi/IDasPluginPackage.h>
#include <cstdint>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    IpcTestPlugin,
    0x1a2b3c4d,
    0x5e6f,
    0x4a5b,
    0x8c,
    0x9d,
    0x0e,
    0x1f,
    0x2a,
    0x3b,
    0x4c,
    0x5d);

DAS_NS_BEGIN

class IpcTestPlugin final
    : public PluginInterface::DasPluginPackageImplBase<IpcTestPlugin>
{
public:
    DAS_IMPL EnumFeature(
        const size_t                       index,
        PluginInterface::DasPluginFeature* p_out_feature) override;
    DAS_IMPL CreateFeatureInterface(size_t index, void** pp_out_interface)
        override;
    DAS_IMPL CanUnloadNow(bool* p_can_unload) override;
};

void IpcTestPluginAddRef();
void IpcTestPluginRelease();

DAS_NS_END

#endif
