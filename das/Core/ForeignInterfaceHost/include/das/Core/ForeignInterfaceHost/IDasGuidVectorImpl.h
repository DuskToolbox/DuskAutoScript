#ifndef DAS_CORE_FOREIGNINTERFACEHOST_DASIIDVECTOR_H
#define DAS_CORE_FOREIGNINTERFACEHOST_DASIIDVECTOR_H

#include <DAS/_autogen/idl/abi/IDasGuidVector.h>
#include <DAS/_autogen/idl/wrapper/Das.ExportInterface.IDasGuidVector.Implements.hpp>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Utils/Expected.h>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class DasGuidVectorImpl final : public ExportInterface::IDasGuidVector,
                                public ExportInterface::IDasReadOnlyGuidVector
{
    std::atomic_uint32_t ref_count_{1};
    std::vector<DasGuid> iids_{};

public:
    DasGuidVectorImpl() = default;
    explicit DasGuidVectorImpl(const std::vector<DasGuid>& iids);

    // IDasBase
    uint32_t DAS_STD_CALL AddRef() override;
    uint32_t DAS_STD_CALL Release() override;
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_out_object) override;
    // IDasGuidVector implementation
    DAS_IMPL Size(size_t* p_out_size) override;
    DAS_IMPL At(size_t index, DasGuid* p_out_iid) override;
    DAS_IMPL Find(const DasGuid& iid) override;
    DAS_IMPL PushBack(const DasGuid& iid) override;
    DAS_IMPL ToConst(
        ExportInterface::IDasReadOnlyGuidVector** pp_out_object) override;

    auto GetImpl() noexcept -> std::vector<DasGuid>&;
    auto ToConst() noexcept -> Utils::Expected<DasPtr<DasGuidVectorImpl>>;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_DASIIDVECTOR_H
