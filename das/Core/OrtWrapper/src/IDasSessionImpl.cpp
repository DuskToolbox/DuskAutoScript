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
        uint32_t input_count = 0;
        input_names->GetCount(&input_count);
        uint32_t output_count = 0;
        output_names->GetCount(&output_count);

        // Use IoBinding to pass tensor references without copying
        Ort::IoBinding io_binding{session_};

        // Bind inputs
        for (uint32_t i = 0; i < input_count; ++i)
        {
            Das::DasPtr<ExportInterface::IDasTensor> p_tensor;
            inputs->GetAt(i, p_tensor.Put());
            auto* tensor_impl = static_cast<IDasTensorImpl*>(p_tensor.Get());

            Das::DasPtr<IDasReadOnlyString> p_name;
            input_names->GetAt(i, p_name.Put());
            DasReadOnlyString name_wrapper(p_name.Get());

            io_binding.BindInput(
                name_wrapper.GetUtf8(),
                tensor_impl->GetOrtValue());
        }

        // Bind outputs — let ORT allocate output memory
        for (uint32_t i = 0; i < output_count; ++i)
        {
            Das::DasPtr<IDasReadOnlyString> p_name;
            output_names->GetAt(i, p_name.Put());
            DasReadOnlyString name_wrapper(p_name.Get());

            io_binding.BindOutput(name_wrapper.GetUtf8(), allocator_.GetInfo());
        }

        // Run inference
        session_.Run(Ort::RunOptions{nullptr}, io_binding);

        // Retrieve outputs
        auto outputs = io_binding.GetOutputValues();

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
