#ifndef ASR_CORE_FOREIGNINTERFACEHOST_COMPONENTFACTORYMANAGER_H
#define ASR_CORE_FOREIGNINTERFACEHOST_COMPONENTFACTORYMANAGER_H

#include <AutoStarRail/Core/ForeignInterfaceHost/Config.h>
#include <AutoStarRail/PluginInterface/IAsrComponent.h>
#include <variant>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class ComponentFactoryManager
{
public:
    using Type = std::
        variant<AsrPtr<IAsrComponentFactory>, AsrPtr<IAsrSwigComponentFactory>>;

private:
    std::vector<Type> common_component_factory_vector_;

    auto FindSupportedFactory(const AsrGuid& iid)
        -> decltype(common_component_factory_vector_)::const_iterator;

public:
    AsrResult Register(IAsrComponentFactory* p_factory);
    AsrResult Register(IAsrSwigComponentFactory* p_factory);

    AsrResult CreateObject(
        const AsrGuid&  iid,
        IAsrComponent** pp_out_component);
    AsrRetComponent CreateObject(const AsrGuid& iid);
};

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // ASR_CORE_FOREIGNINTERFACEHOST_COMPONENTFACTORYMANAGER_H
