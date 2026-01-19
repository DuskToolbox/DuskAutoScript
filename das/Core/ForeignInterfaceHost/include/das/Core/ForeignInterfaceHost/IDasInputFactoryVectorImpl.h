#ifndef DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTFACTORYVECTORIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTFACTORYVECTORIMPL_H

#include <DAS/_autogen/idl/abi/IDasInputFactoryVector.h>
#include <DAS/_autogen/idl/wrapper/Das.ExportInterface.IDasInputFactoryVector.Implements.hpp>
#include <DAS/_autogen/idl/wrapper/Das.ExportInterface.IDasInputFactoryVector.hpp>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/InputFactoryManager.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

using namespace DAS::ExportInterface;

class DasInputFactoryVectorImpl final
    : public DasInputFactoryVectorImplBase<DasInputFactoryVectorImpl>
{
    using Container = std::vector<DasInputFactory>;
    using ContainerIt = typename Container::const_iterator;

    Container input_factory_vector_;

    auto InternalFind(const DasGuid& iid) -> ContainerIt;

public:
    DasInputFactoryVectorImpl(const InputFactoryManager& InputFactoryManager);

    // IDasInputFactoryVector implementation
    DAS_IMPL Size(size_t* p_out_size) override;
    DAS_IMPL At(size_t index, IDasInputFactory** pp_out_factory) override;
    DAS_IMPL Find(const DasGuid& iid, IDasInputFactory** pp_out_factory)
        override;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTFACTORYVECTORIMPL_H
