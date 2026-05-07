#ifndef DAS_CORE_ORTWRAPPER_IDASTENSORVECTORIMPL_H
#define DAS_CORE_ORTWRAPPER_IDASTENSORVECTORIMPL_H

#include "Config.h"
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasTensorVector.Implements.hpp>
#include <vector>

// {C8D1D27A-F753-465D-8F48-1388C4373A5F}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OrtWrapper,
    IDasTensorVectorImpl,
    0xc8d1d27a,
    0xf753,
    0x465d,
    0x8f,
    0x48,
    0x13,
    0x88,
    0xc4,
    0x37,
    0x3a,
    0x5f);

DAS_CORE_ORTWRAPPER_NS_BEGIN

class IDasTensorVectorImpl final
    : public Das::ExportInterface::DasTensorVectorImplBase<IDasTensorVectorImpl>
{
    std::vector<Das::DasPtr<ExportInterface::IDasTensor>> items_;

public:
    IDasTensorVectorImpl() = default;

    DAS_IMPL GetCount(uint32_t* p_count) override;
    DAS_IMPL GetAt(uint32_t index, ExportInterface::IDasTensor** pp_out_value)
        override;

    void AddTensor(ExportInterface::IDasTensor* p_tensor);
    void Reserve(size_t count);
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_IDASTENSORVECTORIMPL_H
