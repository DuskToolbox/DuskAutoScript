#include "IDasTensorImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>
#include <onnxruntime_c_api.h>

#include <cmath>
#include <limits>

DAS_CORE_ORTWRAPPER_NS_BEGIN

namespace
{
    bool TryMultiplyPositiveInt64(
        int64_t  lhs,
        int64_t  rhs,
        int64_t* p_out_value)
    {
        if (lhs <= 0 || rhs <= 0)
        {
            return false;
        }
        if (lhs > std::numeric_limits<int64_t>::max() / rhs)
        {
            return false;
        }
        *p_out_value = lhs * rhs;
        return true;
    }

    bool TryMultiplyUint64(
        uint64_t  lhs,
        uint64_t  rhs,
        uint64_t* p_out_value)
    {
        if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs)
        {
            return false;
        }
        *p_out_value = lhs * rhs;
        return true;
    }

    DasResult GetPixelFormatChannelCount(
        ExportInterface::DasImagePixelFormat pixel_format,
        uint32_t*                            p_out_channel_count)
    {
        DAS_UTILS_CHECK_POINTER(p_out_channel_count);

        switch (pixel_format)
        {
        case ExportInterface::DAS_PIXEL_FORMAT_BGR:
        case ExportInterface::DAS_PIXEL_FORMAT_RGB:
        case ExportInterface::DAS_PIXEL_FORMAT_HSV:
            *p_out_channel_count = 3;
            return DAS_S_OK;
        case ExportInterface::DAS_PIXEL_FORMAT_RGBA:
            *p_out_channel_count = 4;
            return DAS_S_OK;
        case ExportInterface::DAS_PIXEL_FORMAT_GRAY:
            *p_out_channel_count = 1;
            return DAS_S_OK;
        default:
            DAS_CORE_LOG_ERROR(
                "CreateTensorFromImage failed: unsupported pixel_format={}",
                static_cast<int>(pixel_format));
            return DAS_E_INVALID_ENUM;
        }
    }

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

DasResult CreateFloatTensorBackingBufferFromImage(
    ExportInterface::IDasImage* image,
    const int64_t*              shape,
    uint32_t                    rank,
    const double*               mean,
    const double*               stddev,
    uint32_t                    value_count,
    FloatTensorBackingBuffer*   p_out_backing)
{
    DAS_UTILS_CHECK_POINTER(image);
    DAS_UTILS_CHECK_POINTER(shape);
    DAS_UTILS_CHECK_POINTER(mean);
    DAS_UTILS_CHECK_POINTER(stddev);
    DAS_UTILS_CHECK_POINTER(p_out_backing);

    if (rank != 4)
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: expected NCHW rank 4, got {}",
            rank);
        return DAS_E_INVALID_ARGUMENT;
    }

    int64_t total_elements = 1;
    for (uint32_t i = 0; i < rank; ++i)
    {
        if (shape[i] <= 0)
        {
            DAS_CORE_LOG_ERROR(
                "CreateTensorFromImage failed: shape[{}] must be positive, "
                "got {}",
                i,
                shape[i]);
            return DAS_E_INVALID_SIZE;
        }
        if (!TryMultiplyPositiveInt64(total_elements, shape[i], &total_elements))
        {
            DAS_CORE_LOG_ERROR(
                "CreateTensorFromImage failed: shape product overflow at "
                "index {}, value={}",
                i,
                shape[i]);
            return DAS_E_INVALID_SIZE;
        }
    }

    if (shape[0] != 1)
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: only single-image batches are "
            "supported, N={}",
            shape[0]);
        return DAS_E_INVALID_ARGUMENT;
    }

    if (shape[1] > std::numeric_limits<uint32_t>::max())
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: channel dimension too large, C={}",
            shape[1]);
        return DAS_E_INVALID_SIZE;
    }
    const auto tensor_channel_count = static_cast<uint32_t>(shape[1]);
    if (value_count != tensor_channel_count)
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: value_count {} must match tensor "
            "channels {}",
            value_count,
            tensor_channel_count);
        return DAS_E_INVALID_ARGUMENT;
    }

    for (uint32_t c = 0; c < tensor_channel_count; ++c)
    {
        if (!std::isfinite(mean[c]) || !std::isfinite(stddev[c]))
        {
            DAS_CORE_LOG_ERROR(
                "CreateTensorFromImage failed: normalization value at channel "
                "{} is not finite, mean={}, std={}",
                c,
                mean[c],
                stddev[c]);
            return DAS_E_INVALID_ARGUMENT;
        }
        if (stddev[c] == 0.0)
        {
            DAS_CORE_LOG_ERROR(
                "CreateTensorFromImage failed: std at channel {} is zero",
                c);
            return DAS_E_INVALID_ARGUMENT;
        }
    }

    ExportInterface::DasSize image_size{};
    auto result = image->GetSize(&image_size);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: GetSize returned {}",
            result);
        return result;
    }
    if (image_size.width <= 0 || image_size.height <= 0)
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: invalid image size {}x{}",
            image_size.width,
            image_size.height);
        return DAS_E_INVALID_SIZE;
    }
    if (image_size.height != shape[2] || image_size.width != shape[3])
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: image size {}x{} does not match "
            "tensor WxH {}x{}",
            image_size.width,
            image_size.height,
            shape[3],
            shape[2]);
        return DAS_E_INVALID_SIZE;
    }

    int32_t image_channel_count_raw = 0;
    result = image->GetChannelCount(&image_channel_count_raw);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: GetChannelCount returned {}",
            result);
        return result;
    }
    if (image_channel_count_raw <= 0)
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: invalid image channel count {}",
            image_channel_count_raw);
        return DAS_E_INVALID_ARGUMENT;
    }
    const auto image_channel_count =
        static_cast<uint32_t>(image_channel_count_raw);

    ExportInterface::DasImagePixelFormat pixel_format =
        ExportInterface::DAS_PIXEL_FORMAT_UNKNOWN;
    result = image->GetPixelFormat(&pixel_format);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: GetPixelFormat returned {}",
            result);
        return result;
    }

    uint32_t format_channel_count = 0;
    result = GetPixelFormatChannelCount(pixel_format, &format_channel_count);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (image_channel_count != format_channel_count)
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: image channel count {} does not "
            "match pixel_format {} channel count {}",
            image_channel_count,
            static_cast<int>(pixel_format),
            format_channel_count);
        return DAS_E_INVALID_ARGUMENT;
    }
    if (tensor_channel_count > image_channel_count)
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: tensor channels {} exceed image "
            "channels {}",
            tensor_channel_count,
            image_channel_count);
        return DAS_E_INVALID_ARGUMENT;
    }

    uint64_t pixel_count = 0;
    if (!TryMultiplyUint64(
            static_cast<uint64_t>(image_size.width),
            static_cast<uint64_t>(image_size.height),
            &pixel_count))
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: image pixel count overflow, "
            "size={}x{}",
            image_size.width,
            image_size.height);
        return DAS_E_INVALID_SIZE;
    }

    uint64_t required_image_bytes = 0;
    if (!TryMultiplyUint64(
            pixel_count,
            static_cast<uint64_t>(image_channel_count),
            &required_image_bytes))
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: image byte count overflow, "
            "pixels={}, channels={}",
            pixel_count,
            image_channel_count);
        return DAS_E_INVALID_SIZE;
    }

    uint64_t image_data_size = 0;
    result = image->GetDataSize(&image_data_size);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: GetDataSize returned {}",
            result);
        return result;
    }
    if (image_data_size != required_image_bytes)
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: image data size mismatch, "
            "required={}, "
            "actual={}",
            required_image_bytes,
            image_data_size);
        return DAS_E_INVALID_SIZE;
    }

    DAS::DasPtr<ExportInterface::IDasBinaryBuffer> buffer;
    result = image->GetBinaryBuffer(buffer.Put());
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: GetBinaryBuffer returned {}",
            result);
        return result;
    }

    unsigned char* raw_data = nullptr;
    result = buffer->GetData(&raw_data);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: buffer GetData returned {}",
            result);
        return result;
    }
    if (raw_data == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: buffer GetData returned null");
        return DAS_E_INVALID_POINTER;
    }

    uint64_t buffer_size = 0;
    result = buffer->GetSize(&buffer_size);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: buffer GetSize returned {}",
            result);
        return result;
    }
    if (buffer_size < required_image_bytes)
    {
        DAS_CORE_LOG_ERROR(
            "CreateTensorFromImage failed: binary buffer too small, "
            "required={}, actual={}",
            required_image_bytes,
            buffer_size);
        return DAS_E_INVALID_SIZE;
    }

    FloatTensorBackingBuffer backing{};
    result = CreateFloatTensorBackingBuffer(total_elements, &backing);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    auto* const float_data = backing.data;
    const auto  height = static_cast<uint64_t>(shape[2]);
    const auto  width = static_cast<uint64_t>(shape[3]);
    for (uint64_t h = 0; h < height; ++h)
    {
        for (uint64_t w = 0; w < width; ++w)
        {
            const auto pixel_idx = h * width + w;
            for (uint32_t c = 0; c < tensor_channel_count; ++c)
            {
                const auto src_idx =
                    pixel_idx * image_channel_count + static_cast<uint64_t>(c);
                const auto dst_idx =
                    static_cast<size_t>(c) * static_cast<size_t>(pixel_count)
                    + static_cast<size_t>(pixel_idx);
                const auto src_byte = static_cast<double>(raw_data[src_idx]);
                float_data[dst_idx] = static_cast<float>(
                    (src_byte / 255.0 - mean[c]) / stddev[c]);
            }
        }
    }

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
