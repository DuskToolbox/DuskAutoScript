#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>

#include <das/Core/Logger/Logger.h>
#include <new>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// --- GraphRuntimeImpl implementation ---

DasResult GraphRuntimeImpl::GetErrorMessage(
    IDasReadOnlyString** pp_out_error_message)
{
    if (!pp_out_error_message)
    {
        return DAS_E_INVALID_POINTER;
    }

    return CreateIDasReadOnlyStringFromUtf8(
        last_error_.c_str(),
        pp_out_error_message);
}

DAS_CORE_GRAPHRUNTIME_NS_END

// ============================================================================
// C ABI factory function
// ============================================================================

DAS_C_API DasResult
CreateGraphRuntime(Das::ExportInterface::IDasGraphRuntime** pp_out_runtime)
{
    if (!pp_out_runtime)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl = new Das::Core::GraphRuntime::GraphRuntimeImpl{};
        impl->AddRef();
        *pp_out_runtime = impl;
        DAS_CORE_LOG_INFO("CreateGraphRuntime: runtime created successfully");
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("CreateGraphRuntime: out of memory");
        return DAS_E_OUT_OF_MEMORY;
    }
}
