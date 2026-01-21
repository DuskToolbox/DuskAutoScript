#ifndef DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWSCAPTUREFACTORYIMPL_H
#define DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWSCAPTUREFACTORYIMPL_H

#include <DAS/_autogen/idl/abi/IDasCapture.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasCaptureFactory.Implements.hpp>

// {23290FC8-CD40-4C4E-9F58-20EC404F1F3C}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    WindowsCaptureFactoryImpl,
    0x23290fc8,
    0xcd40,
    0x4c4e,
    0x9f,
    0x58,
    0x20,
    0xec,
    0x40,
    0x4f,
    0x1f,
    0x3c)

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
