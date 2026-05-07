#ifndef DAS_CORE_ORTWRAPPER_IDASOCRRESULTVECTORIMPL_H
#define DAS_CORE_ORTWRAPPER_IDASOCRRESULTVECTORIMPL_H

#include "Config.h"
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOcrResultVector.Implements.hpp>
#include <vector>

// {A7C1F3E2-8B5D-4E6A-9C2F-1D3E4B5A6C7D}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OrtWrapper,
    IDasOcrResultVectorImpl,
    0xa7c1f3e2,
    0x8b5d,
    0x4e6a,
    0x9c,
    0x2f,
    0x1d,
    0x3e,
    0x4b,
    0x5a,
    0x6c,
    0x7d);

DAS_CORE_ORTWRAPPER_NS_BEGIN

class IDasOcrResultVectorImpl final
    : public Das::ExportInterface::DasOcrResultVectorImplBase<
          IDasOcrResultVectorImpl>
{
    std::vector<Das::DasPtr<Das::ExportInterface::IDasOcrResult>> items_;

public:
    IDasOcrResultVectorImpl() = default;

    DAS_IMPL GetCount(uint32_t* p_count) override;
    DAS_IMPL GetAt(uint32_t index, Das::ExportInterface::IDasOcrResult** pp_out)
        override;

    void AddResult(Das::ExportInterface::IDasOcrResult* p_result);
    void Reserve(size_t count);
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_IDASOCRRESULTVECTORIMPL_H
