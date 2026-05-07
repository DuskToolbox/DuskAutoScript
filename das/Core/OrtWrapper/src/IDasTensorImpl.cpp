#include "IDasTensorImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>
#include <onnxruntime_c_api.h>

#include <limits>

DAS_CORE_ORTWRAPPER_NS_BEGIN

namespace
{
    size_t GetElementSize(ONNXTensorElementDataType type)
    {
        switch (type)
        {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
            return sizeof(float);
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
            return sizeof(uint8_t);
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
            return sizeof(int8_t);
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:
            return sizeof(uint16_t);
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:
            return sizeof(int16_t);
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
            return sizeof(int32_t);
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
            return sizeof(int64_t);
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
            return 2;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
            return sizeof(double);
        default:
            return 1;
        }
    }
} // namespace

IDasTensorImpl::IDasTensorImpl(Ort::Value value) : value_{std::move(value)} {}

IDasTensorImpl::IDasTensorImpl(
    Ort::Value                  value,
    ExportInterface::IDasImage* image)
    : value_{std::move(value)}, source_image_{image}
{
    if (source_image_)
    {
        source_image_->AddRef();
    }
}

IDasTensorImpl::IDasTensorImpl(
    Ort::Value                         value,
    ExportInterface::IDasMemory*       memory,
    ExportInterface::IDasBinaryBuffer* buffer)
    : value_{std::move(value)}, backing_memory_{memory}, backing_buffer_{buffer}
{
}

DasResult CreateFloatTensorBackingBuffer(
    int64_t                   element_count,
    FloatTensorBackingBuffer* p_out_backing)
{
    DAS_UTILS_CHECK_POINTER(p_out_backing);

    if (element_count <= 0)
    {
        DAS_CORE_LOG_ERROR(
            "CreateFloatTensorBackingBuffer failed: invalid element_count={}",
            element_count);
        return DAS_E_INVALID_ARGUMENT;
    }

    const auto element_count_size = static_cast<size_t>(element_count);
    if (element_count_size > std::numeric_limits<size_t>::max() / sizeof(float))
    {
        DAS_CORE_LOG_ERROR(
            "CreateFloatTensorBackingBuffer failed: element_count overflow, "
            "element_count={}",
            element_count);
        return DAS_E_INVALID_SIZE;
    }

    const auto byte_size = element_count_size * sizeof(float);

    FloatTensorBackingBuffer backing{};
    auto result = ::CreateIDasMemory(byte_size, backing.memory.Put());
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateFloatTensorBackingBuffer failed: CreateIDasMemory({}) "
            "returned {}",
            byte_size,
            result);
        return result;
    }

    result = backing.memory->GetBinaryBuffer(backing.buffer.Put());
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateFloatTensorBackingBuffer failed: GetBinaryBuffer returned {}",
            result);
        return result;
    }

    unsigned char* raw_data = nullptr;
    result = backing.buffer->GetData(&raw_data);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateFloatTensorBackingBuffer failed: GetData returned {}",
            result);
        return result;
    }
    if (raw_data == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "CreateFloatTensorBackingBuffer failed: GetData returned null");
        return DAS_E_INVALID_POINTER;
    }

    uint64_t buffer_size = 0;
    result = backing.buffer->GetSize(&buffer_size);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateFloatTensorBackingBuffer failed: GetSize returned {}",
            result);
        return result;
    }
    if (buffer_size < byte_size)
    {
        DAS_CORE_LOG_ERROR(
            "CreateFloatTensorBackingBuffer failed: backing buffer too small, "
            "required={}, actual={}",
            byte_size,
            buffer_size);
        return DAS_E_INVALID_SIZE;
    }

    backing.data = reinterpret_cast<float*>(raw_data);
    backing.element_count = element_count_size;
    *p_out_backing = std::move(backing);
    return DAS_S_OK;
}

DasResult IDasTensorImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_out_object)
{
    DAS_UTILS_CHECK_POINTER(pp_out_object);

    const auto base_result =
        DAS::ExportInterface::DasTensorImplBase<IDasTensorImpl>::QueryInterface(
            iid,
            pp_out_object);
    if (DAS::IsOk(base_result))
    {
        return base_result;
    }

    if (iid == DasIidOf<IDasTensorImpl>())
    {
        *pp_out_object = static_cast<IDasTensorImpl*>(this);
        AddRef();
        return DAS_S_OK;
    }

    return base_result;
}

DasResult IDasTensorImpl::GetDim(uint32_t index, int64_t* p_value)
{
    DAS_UTILS_CHECK_POINTER(p_value);

    try
    {
        const auto shape = value_.GetTensorTypeAndShapeInfo().GetShape();
        if (index >= shape.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }
        *p_value = shape[index];
        return DAS_S_OK;
    }
    catch (const Ort::Exception& e)
    {
        DAS_CORE_LOG_ERROR("GetDim failed: {}", e.what());
        return DAS_E_ONNX_RUNTIME_ERROR;
    }
}

DasResult IDasTensorImpl::GetRank(uint32_t* p_rank)
{
    DAS_UTILS_CHECK_POINTER(p_rank);

    try
    {
        *p_rank = static_cast<uint32_t>(
            value_.GetTensorTypeAndShapeInfo().GetShape().size());
        return DAS_S_OK;
    }
    catch (const Ort::Exception& e)
    {
        DAS_CORE_LOG_ERROR("GetRank failed: {}", e.what());
        return DAS_E_ONNX_RUNTIME_ERROR;
    }
}

DasResult IDasTensorImpl::GetDataType(
    ExportInterface::DasTensorDataType* p_type)
{
    DAS_UTILS_CHECK_POINTER(p_type);

    try
    {
        *p_type = static_cast<ExportInterface::DasTensorDataType>(
            value_.GetTensorTypeAndShapeInfo().GetElementType());
        return DAS_S_OK;
    }
    catch (const Ort::Exception& e)
    {
        DAS_CORE_LOG_ERROR("GetDataType failed: {}", e.what());
        return DAS_E_ONNX_RUNTIME_ERROR;
    }
}

DasResult IDasTensorImpl::GetRawData(void** pp_data, uint64_t* p_size)
{
    DAS_UTILS_CHECK_POINTER(pp_data);
    DAS_UTILS_CHECK_POINTER(p_size);

    try
    {
        auto shape_info = value_.GetTensorTypeAndShapeInfo();
        *pp_data = value_.GetTensorMutableData<void>();
        const auto element_count = shape_info.GetElementCount();
        const auto element_size = GetElementSize(shape_info.GetElementType());
        *p_size = static_cast<uint64_t>(element_count) * element_size;
        return DAS_S_OK;
    }
    catch (const Ort::Exception& e)
    {
        DAS_CORE_LOG_ERROR("GetRawData failed: {}", e.what());
        return DAS_E_ONNX_RUNTIME_ERROR;
    }
}

DAS_CORE_ORTWRAPPER_NS_END
