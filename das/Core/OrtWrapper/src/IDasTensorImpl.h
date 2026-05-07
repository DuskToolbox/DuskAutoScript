#ifndef DAS_CORE_ORTWRAPPER_IDASTENSORIMPL_H
#define DAS_CORE_ORTWRAPPER_IDASTENSORIMPL_H

#include "DasOrt.h"

#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasTensor.Implements.hpp>

// {95FC46EC-8401-4131-9D87-4FE38243E7F6}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OrtWrapper,
    IDasTensorImpl,
    0x95fc46ec,
    0x8401,
    0x4131,
    0x9d,
    0x87,
    0x4f,
    0xe3,
    0x82,
    0x43,
    0xe7,
    0xf6);

DAS_CORE_ORTWRAPPER_NS_BEGIN

class IDasTensorImpl final
    : public Das::ExportInterface::DasTensorImplBase<IDasTensorImpl>
{
    Ort::Value                              value_;
    Das::DasPtr<ExportInterface::IDasImage> source_image_;

public:
    explicit IDasTensorImpl(Ort::Value value);
    IDasTensorImpl(Ort::Value value, ExportInterface::IDasImage* image);

    DAS_IMPL GetDim(uint32_t index, int64_t* p_value) override;
    DAS_IMPL GetRank(uint32_t* p_rank) override;
    DAS_IMPL GetDataType(ExportInterface::DasTensorDataType* p_type) override;
    DAS_IMPL GetRawData(void** pp_data, uint64_t* p_size) override;

    const Ort::Value& GetOrtValue() const { return value_; }
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_IDASTENSORIMPL_H
