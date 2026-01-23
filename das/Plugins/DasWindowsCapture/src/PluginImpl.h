#ifndef DAS_PLUGINS_DASWINDOWSCAPTURE_PLUGINIMPL_H
#define DAS_PLUGINS_DASWINDOWSCAPTURE_PLUGINIMPL_H

#include <DAS/_autogen/idl/abi/IDasPluginPackage.h>
#include <cstdint>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>
#include <unordered_map>

// {26E90F16-FB71-42C1-BE3C-C5C7721B6D2D}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    WindowsCapturePlugin,
    0x26e90f16,
    0xfb71,
    0x42c1,
    0xbe,
    0x3c,
    0xc5,
    0xc7,
    0x72,
    0x1b,
    0x6d,
    0x2d);

DAS_NS_BEGIN

class WindowsCapturePlugin final
    : public PluginInterface::DasPluginPackageImplBase<WindowsCapturePlugin>
{
public:
    DAS_IMPL EnumFeature(
        const size_t                       index,
        PluginInterface::DasPluginFeature* p_out_feature) override;
    DAS_IMPL CreateFeatureInterface(size_t index, void** pp_out_interface)
        override;
    DAS_IMPL CanUnloadNow() override;
};

void WindowsCaptureAddRef();

void WindowsCaptureRelease();

DAS_NS_END

#endif // DAS_PLUGINS_DASWINDOWSCAPTURE_PLUGINIMPL_H
