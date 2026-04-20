#ifndef DAS_CORE_FOREIGNINTERFACEHOST_DASSTRINGVECTOR_H
#define DAS_CORE_FOREIGNINTERFACEHOST_DASSTRINGVECTOR_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Utils/Expected.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>
#include <string>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class DasStringVectorImpl final : public ExportInterface::IDasStringVector
{
    std::atomic_uint32_t     ref_count_{1};
    std::vector<std::string> strings_{};

public:
    DasStringVectorImpl() = default;
    explicit DasStringVectorImpl(const std::vector<std::string>& strings);

    // IDasBase
    uint32_t DAS_STD_CALL AddRef() override;
    uint32_t DAS_STD_CALL Release() override;
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_out_object) override;

    // IDasStringVector
    DAS_IMPL Size(uint64_t* p_out_size) override;
    DAS_IMPL At(uint64_t index, IDasReadOnlyString** pp_out_string) override;
    DAS_IMPL Find(IDasReadOnlyString* p_string) override;
    DAS_IMPL PushBack(IDasReadOnlyString* p_string) override;

    auto GetImpl() noexcept -> std::vector<std::string>&;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_DASSTRINGVECTOR_H
