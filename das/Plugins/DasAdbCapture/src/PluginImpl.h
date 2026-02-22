#ifndef DAS_PLUGINS_DASADBCAPTURE_PLUGINIMPL_H
#define DAS_PLUGINS_DASADBCAPTURE_PLUGINIMPL_H

#include <cstdint>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <unordered_map>

// {EAC73FD2-5674-4796-8298-71B80727E993}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    AdbCapturePlugin,
    0xeac73fd2,
    0x5674,
    0x4796,
    0x82,
    0x98,
    0x71,
    0xb8,
    0x7,
    0x27,
    0xe9,
    0x93);

DAS_NS_BEGIN

class AdbCapturePlugin final
    : public PluginInterface::DasPluginPackageImplBase<AdbCapturePlugin>
{
public:
    // IDasPluginPackage
    DAS_IMPL EnumFeature(
        const size_t                         index,
        PluginInterface::DasPluginFeature* p_out_feature)
        override;
    DAS_IMPL CreateFeatureInterface(size_t index, void** pp_out_interface)
        override;
    DAS_IMPL CanUnloadNow(bool* p_can_unload) override;
};

void AdbCaptureAddRef();

void AdbCaptureRelease();

DAS_NS_END

#endif // DAS_PLUGINS_DASADBCAPTURE_PLUGINIMPL_H