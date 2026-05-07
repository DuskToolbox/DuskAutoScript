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
        auto cr = input_names->GetCount(&input_count);
        if (DAS::IsFailed(cr))
        {
            DAS_CORE_LOG_ERROR(
                "Session Run failed: input_names->GetCount returned {}",
                cr);
            return cr;
        }

        uint32_t tensor_count = 0;
        cr = inputs->GetCount(&tensor_count);
        if (DAS::IsFailed(cr))
        {
            DAS_CORE_LOG_ERROR(
                "Session Run failed: inputs->GetCount returned {}",
                cr);
            return cr;
        }
        if (tensor_count != input_count)
        {
            DAS_CORE_LOG_ERROR(
                "Session Run failed: input count mismatch, names={}, "
                "tensors={}",
                input_count,
                tensor_count);
            return DAS_E_INVALID_ARGUMENT;
        }

        uint32_t output_count = 0;
        cr = output_names->GetCount(&output_count);
        if (DAS::IsFailed(cr))
        {
            DAS_CORE_LOG_ERROR(
                "Session Run failed: output_names->GetCount returned {}",
                cr);
            return cr;
        }

        // Use IoBinding to pass tensor references without copying
        Ort::IoBinding io_binding{session_};

        // Bind inputs
        for (uint32_t i = 0; i < input_count; ++i)
        {
            DAS::DasPtr<ExportInterface::IDasTensor> p_tensor;
            cr = inputs->GetAt(i, p_tensor.Put());
            if (DAS::IsFailed(cr) || !p_tensor)
            {
                DAS_CORE_LOG_ERROR(
                    "Session Run failed: inputs->GetAt({}) returned {}, "
                    "tensor={}",
                    i,
                    cr,
                    static_cast<bool>(p_tensor));
                return DAS::IsFailed(cr) ? cr : DAS_E_INVALID_POINTER;
            }

            DAS::DasPtr<IDasTensorImpl> tensor_impl;
            cr = p_tensor.As(tensor_impl);
            if (DAS::IsFailed(cr) || !tensor_impl)
            {
                DAS_CORE_LOG_ERROR(
                    "Session Run failed: input tensor {} does not expose "
                    "IDasTensorImpl, result={}",
                    i,
                    cr);
                return DAS::IsFailed(cr) ? cr : DAS_E_NO_INTERFACE;
            }

            DAS::DasPtr<IDasReadOnlyString> p_name;
            cr = input_names->GetAt(i, p_name.Put());
            if (DAS::IsFailed(cr) || !p_name)
            {
                DAS_CORE_LOG_ERROR(
                    "Session Run failed: input_names->GetAt({}) returned {}, "
                    "name={}",
                    i,
                    cr,
                    static_cast<bool>(p_name));
                return DAS::IsFailed(cr) ? cr : DAS_E_INVALID_POINTER;
            }
            DasReadOnlyString name_wrapper(p_name.Get());

            io_binding.BindInput(
                name_wrapper.GetUtf8(),
                tensor_impl->GetOrtValue());
        }

        // Bind outputs — let ORT allocate output memory
        // If caller provided no output names, fall back to names discovered
        // at session construction time (Pitfall 2)
        auto bind_outputs = [&]() -> DasResult
        {
            for (uint32_t i = 0; i < output_count; ++i)
            {
                DAS::DasPtr<IDasReadOnlyString> p_name;
                const auto get_name_result = output_names->GetAt(i, p_name.Put());
                if (DAS::IsFailed(get_name_result) || !p_name)
                {
                    DAS_CORE_LOG_ERROR(
                        "Session Run failed: output_names->GetAt({}) returned "
                        "{}, name={}",
                        i,
                        get_name_result,
                        static_cast<bool>(p_name));
                    return DAS::IsFailed(get_name_result)
                               ? get_name_result
                               : DAS_E_INVALID_POINTER;
                }
                DasReadOnlyString name_wrapper(p_name.Get());
                io_binding.BindOutput(
                    name_wrapper.GetUtf8(),
                    allocator_.GetInfo());
            }
            if (output_count == 0)
            {
                for (const auto& name : output_names_)
                {
                    io_binding.BindOutput(
                        name.c_str(),
                        allocator_.GetInfo());
                }
            }
            return DAS_S_OK;
        };
        cr = bind_outputs();
        if (DAS::IsFailed(cr))
        {
            return cr;
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
