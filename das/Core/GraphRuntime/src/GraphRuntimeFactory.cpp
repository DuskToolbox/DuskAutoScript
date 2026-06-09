#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>

#include <das/Core/Logger/Logger.h>
#include <new>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// --- GraphRuntimeImpl implementation ---

DasResult GraphRuntimeImpl::Load(IDasReadOnlyString* p_compiled_artifact_json)
{
    std::ignore = p_compiled_artifact_json;
    DAS_CORE_LOG_WARN(
        "Load() is deprecated — use GraphRuntime::RunWithHost() directly. "
        "GraphRuntimeImpl is now a minimal COM facade.");
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult GraphRuntimeImpl::Configure(IDasReadOnlyString* p_node_snapshots_json)
{
    std::ignore = p_node_snapshots_json;
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult GraphRuntimeImpl::Run(
    Das::PluginInterface::IDasStopToken* p_stop_token)
{
    std::ignore = p_stop_token;
    last_error_ =
        "Run() is deprecated — use GraphRuntime::RunWithHost() instead";
    return DAS_E_NO_IMPLEMENTATION;
}

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
