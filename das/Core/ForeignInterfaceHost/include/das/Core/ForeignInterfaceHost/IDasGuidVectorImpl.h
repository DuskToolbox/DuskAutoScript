#ifndef DAS_CORE_FOREIGNINTERFACEHOST_DASIIDVECTOR_H
#define DAS_CORE_FOREIGNINTERFACEHOST_DASIIDVECTOR_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/ExportInterface/IDasGuidVector.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class DasGuidVectorImpl;

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

class IDasReadOnlyGuidVectorImpl final : public IDasReadOnlyGuidVector
{
    DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl_;

public:
    IDasReadOnlyGuidVectorImpl(
        DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl);
    // IDasBase
    int64_t   AddRef() override;
    int64_t   Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override;
    // IDasReadOnlyGuidVector
    DasResult Size(size_t* p_out_size) override;
    DasResult At(size_t index, DasGuid* p_out_iid) override;
    DasResult Find(const DasGuid& iid) override;
};

class IDasGuidVectorImpl final : public IDasGuidVector
{
    DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl_;

public:
    IDasGuidVectorImpl(
        DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl);
    // IDasBase
    int64_t   AddRef() override;
    int64_t   Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override;
    // IDasGuidVector
    DasResult Size(size_t* p_out_size) override;
    DasResult At(size_t index, DasGuid* p_out_iid) override;
    DasResult Find(const DasGuid& iid) override;
    DasResult PushBack(const DasGuid& iid) override;
    DasResult ToConst(IDasReadOnlyGuidVector** pp_out_object) override;
    // IDasGuidVectorImpl
    auto GetImpl() noexcept -> std::vector<DasGuid>&;
    auto Get() -> DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl&;
};

class IDasSwigReadOnlyGuidVectorImpl final : public IDasSwigReadOnlyGuidVector
{
    DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl_;

public:
    IDasSwigReadOnlyGuidVectorImpl(
        DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl);
    int64_t        AddRef() override;
    int64_t        Release() override;
    DasRetSwigBase QueryInterface(const DasGuid& iid) override;
    DasRetUInt     Size() override;
    DasRetGuid     At(size_t index) override;
    DasResult      Find(const DasGuid& iid) override;
};

class IDasSwigGuidVectorImpl final : public IDasSwigGuidVector
{
    DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl_;

public:
    IDasSwigGuidVectorImpl(
        DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl);
    int64_t                  AddRef() override;
    int64_t                  Release() override;
    DasRetSwigBase           QueryInterface(const DasGuid& iid) override;
    DasRetUInt               Size() override;
    DasRetGuid               At(size_t index) override;
    DasResult                Find(const DasGuid& iid) override;
    DasResult                PushBack(const DasGuid& iid) override;
    DasRetReadOnlyGuidVector ToConst() override;

    auto Get() -> DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl&;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class DasGuidVectorImpl : DAS_UTILS_MULTIPLE_PROJECTION_GENERATORS(
                              DasGuidVectorImpl,
                              IDasGuidVectorImpl,
                              IDasSwigGuidVectorImpl),
                          DAS_UTILS_MULTIPLE_PROJECTION_GENERATORS(
                              DasGuidVectorImpl,
                              IDasReadOnlyGuidVectorImpl,
                              IDasSwigReadOnlyGuidVectorImpl)
{
    Utils::RefCounter<DasGuidVectorImpl> ref_counter_{};
    std::vector<DasGuid>                 iids_{};

public:
    DasGuidVectorImpl() = default;
    explicit DasGuidVectorImpl(const std::vector<DasGuid>& iids);
    int64_t AddRef();
    int64_t Release();
    auto    Size() const noexcept -> size_t;
    auto    At(size_t index, DasGuid& out_guid) const noexcept -> DasResult;
    auto    Find(const DasGuid guid) noexcept -> DasResult;
    auto    PushBack(const DasGuid guid) noexcept -> DasResult;

    auto GetImpl() noexcept -> std::vector<DasGuid>&;
    auto ToConst() noexcept -> Utils::Expected<DasPtr<DasGuidVectorImpl>>;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_DASIIDVECTOR_H
