#ifndef DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTFACTORYVECTORIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTFACTORYVECTORIMPL_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/InputFactoryManager.h>
#include <das/ExportInterface/IDasInputFactoryVector.h>
#include <das/Utils/CommonUtils.hpp>
#include <variant>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

using CommonInputPtr = std::variant<DasPtr<IDasInput>, DasPtr<IDasSwigInput>>;

class DasInputFactoryVectorImpl;

class IDasInputFactoryVectorImpl final : public IDasInputFactoryVector
{
public:
    explicit IDasInputFactoryVectorImpl(DasInputFactoryVectorImpl& impl);
    // IDasBase
    auto AddRef() -> int64_t override;
    auto Release() -> int64_t override;
    auto QueryInterface(const DasGuid& iid, void** pp_object)
        -> DasResult override;
    // IDasInputFactoryVector
    DAS_IMPL Size(size_t* p_out_size) override;
    DAS_IMPL At(size_t index, IDasInputFactory** pp_out_factory) override;
    DAS_IMPL Find(const DasGuid& iid, IDasInputFactory** pp_out_factory)
        override;

private:
    DasInputFactoryVectorImpl& impl_;
};

class IDasSwigInputFactoryVectorImpl final : public IDasSwigInputFactoryVector
{
public:
    explicit IDasSwigInputFactoryVectorImpl(DasInputFactoryVectorImpl& impl);
    // IDasBase
    auto AddRef() -> int64_t override;
    auto Release() -> int64_t override;
    auto QueryInterface(const DasGuid& iid) -> DasRetSwigBase override;
    // IDasSwigInputFactoryVector
    auto Size() -> DasRetUInt override;
    auto At(size_t index) -> DasRetInputFactory override;
    auto Find(const DasGuid& iid) -> DasRetInputFactory override;

private:
    DasInputFactoryVectorImpl& impl_;
};

class DasInputFactoryVectorImpl final
    : DAS_UTILS_MULTIPLE_PROJECTION_GENERATORS(
          DasInputFactoryVectorImpl,
          IDasInputFactoryVectorImpl,
          IDasSwigInputFactoryVectorImpl)
{
    using Type = typename InputFactoryManager::Type;
    using Container = std::vector<Type>;
    using ContainerIt = typename Container::const_iterator;

    DAS::Utils::RefCounter<DasInputFactoryVectorImpl> ref_counter_{};
    Container                                         input_factory_vector_{};

    auto InternalFind(const DasGuid& iid) -> ContainerIt;

public:
    DasInputFactoryVectorImpl(const InputFactoryManager& InputFactoryManager);
    auto AddRef() -> int64_t;
    auto Release() -> int64_t;
    [[nodiscard]]
    auto Size() const noexcept -> size_t;
    auto At(size_t index, IDasInputFactory** pp_out_factory) const -> DasResult;
    auto Find(const DasGuid& iid, IDasInputFactory** pp_out_factory)
        -> DasResult;
    auto At(size_t index) -> DasRetInputFactory;
    auto Find(const DasGuid& iid) -> DasRetInputFactory;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTFACTORYVECTORIMPL_H
