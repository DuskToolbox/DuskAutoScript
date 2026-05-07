#include "AiCudaImpl.h"
#include "AiCpuImpl.h"
#include "IDasSessionImpl.h"
#include "IDasTensorImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

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
        FloatTensorBackingBuffer backing{};
        auto cr = CreateFloatTensorBackingBufferFromImage(
            image,
            shape,
            rank,
            mean,
            std,
            value_count,
            &backing);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }

        // Create ORT tensor over DAS-owned memory.
        auto input_tensor = Ort::Value::CreateTensor<float>(
            GetDefaultCpuMemoryInfo(),
            backing.data,
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
