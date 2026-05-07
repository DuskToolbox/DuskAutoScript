#include "AiCudaImpl.h"
#include "AiCpuImpl.h"
#include "IDasSessionImpl.h"
#include "IDasTensorImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

#include <algorithm>
#include <vector>

DAS_CORE_ORTWRAPPER_NS_BEGIN

DasResult AiCudaImpl::CreateSession(
    IDasReadOnlyString* model_path,
    ExportInterface::IDasJson* /*options*/,
    ExportInterface::IDasSession** pp_session)
{
    DAS_UTILS_CHECK_POINTER(pp_session);
    DAS_UTILS_CHECK_POINTER(model_path);

    try
    {
        const auto* model_path_ort = ToOrtChar(model_path);

        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Register CUDA execution provider
        OrtCUDAProviderOptions cuda_opts;
        cuda_opts.device_id = 0;
        session_options.AppendExecutionProvider_CUDA(cuda_opts);

        auto session = Ort::Session(GetEnv(), model_path_ort, session_options);

        auto* impl = new IDasSessionImpl(std::move(session));
        impl->AddRef();
        *pp_session = impl;
        return DAS_S_OK;
    }
    catch (const Ort::Exception& e)
    {
        DasReadOnlyString path_wrapper(model_path);
        DAS_CORE_LOG_ERROR(
            "CreateSession (CUDA EP) failed: model_path={}, error={}",
            path_wrapper.GetUtf8(),
            e.what());
        return DAS_E_ONNX_RUNTIME_ERROR;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult AiCudaImpl::CreateTensorFromImage(
    ExportInterface::IDasImage*   image,
    int64_t*                      shape,
    uint32_t                      rank,
    double*                       mean,
    double*                       std,
    uint32_t                      value_count,
    ExportInterface::IDasTensor** pp_tensor)
{
    DAS_UTILS_CHECK_POINTER(pp_tensor);
    DAS_UTILS_CHECK_POINTER(image);
    DAS_UTILS_CHECK_POINTER(shape);
    DAS_UTILS_CHECK_POINTER(mean);
    DAS_UTILS_CHECK_POINTER(std);

    try
    {
        // Get image data via IDasBinaryBuffer
        DAS::DasPtr<ExportInterface::IDasBinaryBuffer> buf;
        auto cr = image->GetBinaryBuffer(buf.Put());
        if (DAS::IsFailed(cr))
        {
            DAS_CORE_LOG_ERROR("GetBinaryBuffer failed: result={}", cr);
            return cr;
        }

        unsigned char* raw_data = nullptr;
        buf->GetData(&raw_data);
        uint64_t data_size = 0;
        buf->GetSize(&data_size);

        // Calculate total element count from shape
        int64_t total_elements = 1;
        for (uint32_t i = 0; i < rank; ++i)
        {
            total_elements *= shape[i];
        }

        // Materialize: preprocess pixel data to DAS-owned tensor memory.
        FloatTensorBackingBuffer backing{};
        cr = CreateFloatTensorBackingBuffer(total_elements, &backing);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }
        auto* float_data = backing.data;
        std::fill_n(float_data, backing.element_count, 0.0f);

        // Simple preprocessing: normalize each pixel channel
        // Shape is expected to be [N, C, H, W] (CHW format)
        // value_count = number of channels (typically 3)
        if (rank >= 3 && value_count > 0)
        {
            auto channel_count = static_cast<uint32_t>(shape[rank - 3]);
            auto height = static_cast<uint64_t>(shape[rank - 2]);
            auto width = static_cast<uint64_t>(shape[rank - 1]);
            auto pixel_count = height * width;
            auto bytes_per_pixel = data_size / pixel_count;

            for (uint64_t h = 0; h < height; ++h)
            {
                for (uint64_t w = 0; w < width; ++w)
                {
                    for (uint32_t c = 0; c < channel_count && c < value_count;
                         ++c)
                    {
                        if (c < bytes_per_pixel)
                        {
                            auto pixel_idx = h * width + w;
                            auto src_byte = static_cast<double>(
                                raw_data[pixel_idx * bytes_per_pixel + c]);
                            float val = static_cast<float>(
                                (src_byte / 255.0 - mean[c]) / std[c]);
                            auto dst_idx = c * pixel_count + h * width + w;
                            if (dst_idx < backing.element_count)
                            {
                                float_data[dst_idx] = val;
                            }
                        }
                    }
                }
            }
        }
        else
        {
            // Fallback: raw byte copy
            for (int64_t i = 0;
                 i < total_elements && i < static_cast<int64_t>(data_size);
                 ++i)
            {
                float_data[i] = static_cast<float>(raw_data[i]);
            }
        }

        // Create ORT tensor over DAS-owned memory.
        auto input_tensor = Ort::Value::CreateTensor<float>(
            GetDefaultCpuMemoryInfo(),
            float_data,
            backing.element_count,
            shape,
            rank);

        auto* impl = new IDasTensorImpl(
            std::move(input_tensor),
            backing.memory.Get(),
            backing.buffer.Get());
        impl->AddRef();
        *pp_tensor = impl;
        return DAS_S_OK;
    }
    catch (const Ort::Exception& e)
    {
        DAS_CORE_LOG_ERROR("CreateTensorFromImage failed: {}", e.what());
        return DAS_E_ONNX_RUNTIME_ERROR;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult AiCudaImpl::CreateOcr(
    IDasReadOnlyString*        det_model,
    IDasReadOnlyString*        rec_model,
    IDasReadOnlyString*        dict_path,
    ExportInterface::IDasOcr** pp_ocr)
{
    return CreateOcrImpl(this, det_model, rec_model, dict_path, pp_ocr);
}

DAS_CORE_ORTWRAPPER_NS_END
