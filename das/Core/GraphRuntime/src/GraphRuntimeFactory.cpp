#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>

#include <cpp_yyjson.hpp>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>
#include <new>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// --- GraphRuntimeImpl implementation ---

DasResult GraphRuntimeImpl::Load(IDasReadOnlyString* p_compiled_artifact_json)
{
    if (!p_compiled_artifact_json)
    {
        last_error_ = "IDasReadOnlyString pointer is null";
        return DAS_E_INVALID_POINTER;
    }

    const char* json_str = nullptr;
    DasResult   hr = p_compiled_artifact_json->GetUtf8(&json_str);
    if (DAS::IsFailed(hr) || !json_str)
    {
        last_error_ = "Failed to read compiled artifact JSON string";
        return DAS_E_INVALID_ARGUMENT;
    }

    // Parse JSON → CompiledGraphPlanDto via yyjson casters and delegate to
    // engine.
    Dto::CompiledGraphPlanDto plan;
    try
    {
        auto doc = yyjson::read(json_str);
        plan = yyjson::cast<Dto::CompiledGraphPlanDto>(doc);
    }
    catch (const std::exception& e)
    {
        last_error_ = DAS_FMT_NS::format(
            "Failed to parse compiled artifact: {}",
            e.what());
        return DAS_E_INVALID_JSON;
    }

    // Store plan for later Configure/Run calls.
    hr = engine_.Configure(plan, nullptr);
    if (DAS::IsFailed(hr))
    {
        last_error_ = engine_.GetLastErrorMessage();
    }
    return hr;
}

DasResult GraphRuntimeImpl::Configure(IDasReadOnlyString* p_node_snapshots_json)
{
    // Configure is already handled inside Load for this implementation.
    // A separate Configure with node snapshot overrides is for future use.
    std::ignore = p_node_snapshots_json;
    return DAS_S_OK;
}

DasResult GraphRuntimeImpl::Run(
    Das::PluginInterface::IDasStopToken* p_stop_token)
{
    // Delegate to engine's Prepare — the engine was already configured in
    // Load().
    DasResult hr = engine_.Prepare();
    if (DAS::IsFailed(hr))
    {
        last_error_ = engine_.GetLastErrorMessage();
        return hr;
    }

    // Check stop token before execution.
    if (p_stop_token)
    {
        bool can_stop = false;
        hr = p_stop_token->StopRequested(&can_stop);
        if (DAS::IsOk(hr) && can_stop)
        {
            last_error_ = "Execution cancelled by stop token";
            return DAS_E_FAIL;
        }
    }

    return DAS_S_OK;
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
