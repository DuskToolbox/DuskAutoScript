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

    // Cache the parsed plan — do NOT call engine_.Configure() here because
    // p_host is unavailable at this point.  The caller must provide a host
    // later (e.g., via RunWithHost) to actually execute the graph.
    cached_plan_ = std::move(plan);
    plan_loaded_ = true;
    return DAS_S_OK;
}

DasResult GraphRuntimeImpl::Configure(IDasReadOnlyString* p_node_snapshots_json)
{
    if (p_node_snapshots_json)
    {
        DAS_CORE_LOG_WARN(
            "Configure() with node snapshot overrides is not implemented. "
            "Node snapshots are configured during Load().");
        return DAS_E_NO_IMPLEMENTATION;
    }
    return DAS_S_OK;
}

DasResult GraphRuntimeImpl::Run(
    Das::PluginInterface::IDasStopToken* p_stop_token)
{
    if (!plan_loaded_)
    {
        last_error_ = "No compiled artifact loaded — call Load() first";
        return DAS_E_FAIL;
    }

    // Delegate to engine's Run() with a minimal ComponentResolver.
    // The resolver is a placeholder — in production, components should be
    // resolved via RunWithHost() using an IDasTaskComponentHost.
    DasResult hr = engine_.Run(
        cached_plan_,
        cached_plan_.source_fingerprint,
        p_stop_token,
        /* resolve_component */
        [](const std::string&                   component_guid,
           Das::PluginInterface::IDasStopToken* p_stop,
           Das::ExportInterface::IDasPortMap*   input_map,
           Das::ExportInterface::IDasPortMap**  pp_out_map) -> DasResult
        {
            std::ignore = component_guid;
            std::ignore = p_stop;
            std::ignore = input_map;
            std::ignore = pp_out_map;
            return DAS_E_NO_IMPLEMENTATION;
        });
    if (DAS::IsFailed(hr))
    {
        last_error_ = engine_.GetLastErrorMessage();
    }
    return hr;
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
