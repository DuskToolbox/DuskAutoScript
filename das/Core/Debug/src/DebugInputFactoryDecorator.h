#ifndef DAS_CORE_DEBUG_DEBUGINPUTFACTORYDECORATOR_H
#define DAS_CORE_DEBUG_DEBUGINPUTFACTORYDECORATOR_H

#include <das/Core/Debug/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasInputFactory.Implements.hpp>

#include <string>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::Debug,
    DebugInputFactoryDecorator,
    0xc4355d49,
    0x10d3,
    0x46b9,
    0xa4,
    0x4c,
    0x25,
    0xf4,
    0x65,
    0xc1,
    0xee,
    0x9a);

DAS_CORE_DEBUG_NS_BEGIN

class DebugInputFactoryDecorator final
    : public Das::PluginInterface::DasInputFactoryImplBase<
          DebugInputFactoryDecorator>
{
public:
    DebugInputFactoryDecorator(
        Das::PluginInterface::IDasInputFactory* p_inner,
        const char*                             p_factory_name);

    DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override;
    DasResult DAS_STD_CALL
    GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;
    DasResult DAS_STD_CALL CreateInstance(
        IDasReadOnlyString*                 p_json_config,
        Das::PluginInterface::IDasInput**   pp_out_input) override;

private:
    Das::DasPtr<Das::PluginInterface::IDasInputFactory> inner_;
    std::string                                         factory_name_;
};

DAS_CORE_DEBUG_NS_END

#endif // DAS_CORE_DEBUG_DEBUGINPUTFACTORYDECORATOR_H
