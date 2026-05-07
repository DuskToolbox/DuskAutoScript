#include "AiCpuImpl.h"
#include "IDasSessionImpl.h"
#include "IDasTensorImpl.h"
#include "PaddleOcrImpl.h"

#include <das/Core/Debug/DebugDecorators.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

#include <fstream>
#include <vector>

DAS_CORE_ORTWRAPPER_NS_BEGIN

DasResult AiCpuImpl::CreateSession(
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

        // CPU EP is default — no explicit provider registration needed
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
            "CreateSession failed: model_path={}, error={}",
            path_wrapper.GetUtf8(),
            e.what());
        return DAS_E_ONNX_RUNTIME_ERROR;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult AiCpuImpl::CreateTensorFromImage(
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

DasResult AiCpuImpl::CreateOcr(
    IDasReadOnlyString*        det_model,
    IDasReadOnlyString*        rec_model,
    IDasReadOnlyString*        dict_path,
    ExportInterface::IDasOcr** pp_ocr)
{
    return CreateOcrImpl(this, det_model, rec_model, dict_path, pp_ocr);
}

// Shared CreateOcr implementation
DasResult CreateOcrImpl(
    ExportInterface::IDasAI*   ai,
    IDasReadOnlyString*        det_model,
    IDasReadOnlyString*        rec_model,
    IDasReadOnlyString*        dict_path,
    ExportInterface::IDasOcr** pp_ocr)
{
    DAS_UTILS_CHECK_POINTER(pp_ocr);
    DAS_UTILS_CHECK_POINTER(rec_model);
    DAS_UTILS_CHECK_POINTER(dict_path);
    // det_model can be nullptr -> only_rec mode (D-14)

    try
    {
        // 1. Load dictionary (D-22)
        std::vector<std::string> dict;
        {
            const char* dict_utf8 = nullptr;
            dict_path->GetUtf8(&dict_utf8);
            if (!dict_utf8)
            {
                return DAS_E_INVALID_POINTER;
            }
            std::ifstream dict_file(dict_utf8);
            if (!dict_file.is_open())
            {
                DAS_CORE_LOG_ERROR("Failed to open dict file: {}", dict_utf8);
                return DAS_E_FILE_NOT_FOUND;
            }
            std::string line;
            while (std::getline(dict_file, line))
            {
                dict.push_back(line);
            }
        }

        if (dict.empty())
        {
            DAS_CORE_LOG_ERROR("Dictionary is empty");
            return DAS_E_INVALID_FILE;
        }

        // 2. Create rec session (required)
        DAS::DasPtr<ExportInterface::IDasSession> rec_session;
        auto result = ai->CreateSession(rec_model, nullptr, rec_session.Put());
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_ERROR(
                "CreateOcr: rec session failed, result={}",
                result);
            return result;
        }

        // 3. Create det session (optional, D-14)
        DAS::DasPtr<ExportInterface::IDasSession> det_session;
        if (det_model)
        {
            result = ai->CreateSession(det_model, nullptr, det_session.Put());
            if (DAS::IsFailed(result))
            {
                DAS_CORE_LOG_ERROR(
                    "CreateOcr: det session failed, result={}",
                    result);
                return result;
            }
        }

        // 4. Construct PaddleOcrImpl (D-13: RAII construction)
        auto* impl = new PaddleOcrImpl(
            ai,
            det_session.Get(),
            rec_session.Get(),
            std::move(dict));
        impl->AddRef();
        *pp_ocr = DAS::Core::Debug::MaybeDecorateOcrRaw(impl, "ocr");
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("CreateOcr failed: {}", e.what());
        return DAS_E_FAIL;
    }
}

DAS_CORE_ORTWRAPPER_NS_END
