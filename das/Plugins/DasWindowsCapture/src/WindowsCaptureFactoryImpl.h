#ifndef DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWSCAPTUREFACTORYIMPL_H
#define DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWSCAPTUREFACTORYIMPL_H

#include <DAS/_autogen/idl/abi/IDasCapture.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasCaptureFactory.Implements.hpp>

// {C9225681-DFE3-45D2-B6DA-FE2FC9452513}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    WindowsCaptureFactoryImpl,
    0xc9225681,
    0xdfe3,
    0x45d2,
    0xb6,
    0xda,
    0xfe,
    0x2f,
    0xc9,
    0x45,
    0x25,
    0x13)

DAS_NS_BEGIN

class WindowsCaptureFactoryImpl final
    : public PluginInterface::DasCaptureFactoryImplBase<
          WindowsCaptureFactoryImpl>
{
public:
    WindowsCaptureFactoryImpl();
    ~WindowsCaptureFactoryImpl();

    DAS_IMPL CreateInstance(
        IDasReadOnlyString*            p_environment_json_config,
        IDasReadOnlyString*            p_plugin_config,
        PluginInterface::IDasCapture** pp_object) override;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWSCAPTUREFACTORYIMPL_H
