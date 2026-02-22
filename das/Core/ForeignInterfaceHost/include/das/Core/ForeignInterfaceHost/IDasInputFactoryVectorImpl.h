#ifndef DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTFACTORYVECTORIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTFACTORYVECTORIMPL_H

#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasInputFactoryVector.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasInputFactoryVector.hpp>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/InputFactoryManager.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class DasInputFactoryVectorImpl final
    : public DAS::ExportInterface::DasInputFactoryVectorImplBase<
          DasInputFactoryVectorImpl>
{
    using Container = std::vector<PluginInterface::DasInputFactory>;
    using ContainerIt = typename Container::const_iterator;

    Container input_factory_vector_;

    auto InternalFind(const DasGuid& iid) -> ContainerIt;

public:
    DasInputFactoryVectorImpl(const InputFactoryManager& InputFactoryManager);

    // IDasInputFactoryVector implementation
    DAS_IMPL Size(size_t* p_out_size) override;
    DAS_IMPL At(
        size_t                              index,
        PluginInterface::IDasInputFactory** pp_out_factory) override;
    DAS_IMPL Find(
        const DasGuid&                      iid,
        PluginInterface::IDasInputFactory** pp_out_factory) override;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTFACTORYVECTORIMPL_H
