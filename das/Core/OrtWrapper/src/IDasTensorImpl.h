#ifndef DAS_CORE_ORTWRAPPER_IDASTENSORIMPL_H
#define DAS_CORE_ORTWRAPPER_IDASTENSORIMPL_H

#include "DasOrt.h"

#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>
#include <das/_autogen/idl/abi/IDasImage.h>
#include <das/_autogen/idl/abi/IDasMemory.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasTensor.Implements.hpp>

#include <array>
#include <cstddef>

DAS_NS_BEGIN
namespace Core
{
namespace OrtWrapper
{
    class IDasTensorImpl;
}
} // namespace Core
DAS_NS_END

// {95FC46EC-8401-4131-9D87-4FE38243E7F6}
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    DAS::Core::OrtWrapper,
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

struct FloatTensorBackingBuffer final
{
    DAS::DasPtr<ExportInterface::IDasMemory>       memory;
    DAS::DasPtr<ExportInterface::IDasBinaryBuffer> buffer;
    std::array<int64_t, 4>                         shape{};
    float*                                         data{};
    size_t                                         element_count{};
};

DasResult CreateFloatTensorBackingBuffer(
    int64_t                   element_count,
    FloatTensorBackingBuffer* p_out_backing);

DasResult CreateFloatTensorBackingBufferFromImage(
    ExportInterface::IDasImage*                  image,
    const ExportInterface::DasImageTensorOptions& options,
    FloatTensorBackingBuffer*                    p_out_backing);

class IDasTensorImpl final
    : public DAS::ExportInterface::DasTensorImplBase<IDasTensorImpl>
{
    Ort::Value                              value_;
    DAS::DasPtr<ExportInterface::IDasImage> source_image_;
    DAS::DasPtr<ExportInterface::IDasMemory> backing_memory_;
    DAS::DasPtr<ExportInterface::IDasBinaryBuffer> backing_buffer_;

public:
    explicit IDasTensorImpl(Ort::Value value);
    IDasTensorImpl(Ort::Value value, ExportInterface::IDasImage* image);
    IDasTensorImpl(
        Ort::Value                          value,
        ExportInterface::IDasMemory*        memory,
        ExportInterface::IDasBinaryBuffer*  buffer);

    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_out_object) override;
    DAS_IMPL GetDim(uint32_t index, int64_t* p_value) override;
    DAS_IMPL GetRank(uint32_t* p_rank) override;
    DAS_IMPL GetDataType(ExportInterface::DasTensorDataType* p_type) override;
    DAS_IMPL GetBinaryBuffer(
        ExportInterface::IDasBinaryBuffer** pp_out_buffer) override;

    const Ort::Value& GetOrtValue() const { return value_; }
    ExportInterface::IDasMemory* GetBackingMemory() const
    {
        return backing_memory_.Get();
    }
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_IDASTENSORIMPL_H
