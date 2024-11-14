#ifndef DAS_CORE_FOREIGNINTERFACEHOST_COMPONENTFACTORYMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_COMPONENTFACTORYMANAGER_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/PluginInterface/IDasComponent.h>
#include <variant>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class ComponentFactoryManager
{
public:
    using Type = std::
        variant<DasPtr<IDasComponentFactory>, DasPtr<IDasSwigComponentFactory>>;

private:
    std::vector<Type> common_component_factory_vector_;

    auto FindSupportedFactory(const DasGuid& iid)
        -> decltype(common_component_factory_vector_)::const_iterator;

public:
    DasResult Register(IDasComponentFactory* p_factory);
    DasResult Register(IDasSwigComponentFactory* p_factory);

    DasResult CreateObject(
        const DasGuid&  iid,
        IDasComponent** pp_out_component);
    DasRetComponent CreateObject(const DasGuid& iid);
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_COMPONENTFACTORYMANAGER_H
