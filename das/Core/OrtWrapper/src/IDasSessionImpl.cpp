#include "IDasSessionImpl.h"
#include "IDasTensorImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_ORTWRAPPER_NS_BEGIN

IDasSessionImpl::IDasSessionImpl(Ort::Session session)
    : session_{std::move(session)}
{
    // Discover input/output names at construction time (Pitfall 2)
    auto num_inputs = session_.GetInputCount();
    for (size_t i = 0; i < num_inputs; ++i)
    {
        auto name = session_.GetInputNameAllocated(i, allocator_);
        input_names_.push_back(name.get());
    }

    auto num_outputs = session_.GetOutputCount();
    for (size_t i = 0; i < num_outputs; ++i)
    {
        auto name = session_.GetOutputNameAllocated(i, allocator_);
        output_names_.push_back(name.get());
    }
}

DasResult IDasSessionImpl::Run(
    ExportInterface::IDasReadOnlyStringVector* input_names,
    ExportInterface::IDasTensorVector*         inputs,
    ExportInterface::IDasReadOnlyStringVector* output_names,
    ExportInterface::IDasTensorVector**        pp_outputs)
{
    DAS_UTILS_CHECK_POINTER(pp_outputs);
    DAS_UTILS_CHECK_POINTER(inputs);
    DAS_UTILS_CHECK_POINTER(input_names);
    DAS_UTILS_CHECK_POINTER(output_names);

    try
    {
        // Collect input name strings
        uint32_t input_count = 0;
        input_names->GetCount(&input_count);

        std::vector<std::string> input_name_strs;
        std::vector<const char*> input_name_cstrs;
        input_name_strs.reserve(input_count);
        input_name_cstrs.reserve(input_count);

        for (uint32_t i = 0; i < input_count; ++i)
        {
            Das::DasPtr<IDasReadOnlyString> p_name;
            input_names->GetAt(i, p_name.Put());
            DasReadOnlyString wrapper(p_name.Get());
            input_name_strs.push_back(wrapper.GetUtf8());
            input_name_cstrs.push_back(input_name_strs.back().c_str());
        }

        // Collect input Ort::Value references
        std::vector<const Ort::Value*> ort_values;
        ort_values.reserve(input_count);
        for (uint32_t i = 0; i < input_count; ++i)
        {
            Das::DasPtr<ExportInterface::IDasTensor> p_tensor;
            inputs->GetAt(i, p_tensor.Put());
            auto* tensor_impl = static_cast<IDasTensorImpl*>(p_tensor.Get());
            ort_values.push_back(&tensor_impl->GetOrtValue());
        }

        // Collect output name strings
        uint32_t output_count = 0;
        output_names->GetCount(&output_count);

        std::vector<std::string> output_name_strs;
        std::vector<const char*> output_name_cstrs;
        output_name_strs.reserve(output_count);
        output_name_cstrs.reserve(output_count);

        for (uint32_t i = 0; i < output_count; ++i)
        {
            Das::DasPtr<IDasReadOnlyString> p_name;
            output_names->GetAt(i, p_name.Put());
            DasReadOnlyString wrapper(p_name.Get());
            output_name_strs.push_back(wrapper.GetUtf8());
            output_name_cstrs.push_back(output_name_strs.back().c_str());
        }

        // Run inference
        auto outputs = session_.Run(
            Ort::RunOptions{nullptr},
            input_name_cstrs.data(),
            ort_values.data(),
            input_count,
            output_name_cstrs.data(),
            output_count);

        // Wrap outputs into IDasTensorVector
        auto* result_vector = new IDasTensorVectorImpl{};
        result_vector->AddRef();
        result_vector->Reserve(outputs.size());

        for (auto& output : outputs)
        {
            auto* tensor_impl = new IDasTensorImpl(std::move(output));
            tensor_impl->AddRef();
            result_vector->AddTensor(tensor_impl);
            tensor_impl->Release();
        }

        *pp_outputs = result_vector;
        return DAS_S_OK;
    }
    catch (const Ort::Exception& e)
    {
        DAS_CORE_LOG_ERROR("Session Run failed: {}", e.what());
        return DAS_E_ONNX_RUNTIME_ERROR;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_CORE_ORTWRAPPER_NS_END
