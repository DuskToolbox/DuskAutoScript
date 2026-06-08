#include <das/Core/GraphRuntime/GraphRuntime.h>

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/DoAdapter.h>
#include <das/Core/GraphRuntime/PortFrame.h>
#include <das/Core/GraphRuntime/RuntimeExecutionCache.h>
#include <das/Core/Logger/Logger.h>

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>

#include <algorithm>
#include <string>
#include <vector>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

using IDasPortMap = Das::ExportInterface::IDasPortMap;

// ---------------------------------------------------------------------------
// ValidateFingerprint
// ---------------------------------------------------------------------------
DasResult GraphRuntime::ValidateFingerprint(
    const std::string& compiled_source_fingerprint,
    const std::string& current_fingerprint)
{
    if (compiled_source_fingerprint.empty())
    {
        DAS_CORE_LOG_WARN(
            "Compiled plan has no source_fingerprint — skipping validation");
        return DAS_S_OK;
    }

    if (compiled_source_fingerprint != current_fingerprint)
    {
        DAS_CORE_LOG_ERROR(
            "Fingerprint mismatch: compiled = {}, current = {}",
            compiled_source_fingerprint,
            current_fingerprint);
        return DAS_E_FAIL;
    }

    DAS_CORE_LOG_TRACE(
        "Fingerprint validated: {} = {}",
        compiled_source_fingerprint,
        current_fingerprint);
    return DAS_S_OK;
}

// ---------------------------------------------------------------------------
// CheckStopToken
// ---------------------------------------------------------------------------
DasResult GraphRuntime::CheckStopToken(
    Das::PluginInterface::IDasStopToken* p_stop_token)
{
    if (!p_stop_token)
        return DAS_S_OK;

    bool      stop_requested = false;
    DasResult hr = p_stop_token->StopRequested(&stop_requested);
    if (DAS_S_OK != hr)
    {
        DAS_CORE_LOG_ERROR(
            "StopToken::StopRequested failed: hr = {}",
            static_cast<int>(hr));
        return hr;
    }

    if (stop_requested)
    {
        DAS_CORE_LOG_INFO("Execution cancelled by stop token");
        return DAS_E_FAIL;
    }

    return DAS_S_OK;
}

// ---------------------------------------------------------------------------
// ExecuteNode
// ---------------------------------------------------------------------------
DasResult GraphRuntime::ExecuteNode(
    const std::string&                   node_id,
    const std::string&                   component_guid,
    Das::PluginInterface::IDasStopToken* p_stop_token,
    ComponentResolver&                   resolve_component,
    PortFrame&                           frame,
    const Dto::CompiledGraphPlanDto&     plan)
{
    // 1. Collect input bindings for this target node
    std::vector<Dto::PortBindingDto> input_bindings;
    for (const auto& b : plan.binding_plan.bindings)
    {
        if (b.target_node_id == node_id)
            input_bindings.push_back(b);
    }

    // 2. Build input PortMap from upstream PortFrame data
    DAS::DasPtr<IDasPortMap> input_portmap;

    if (!input_bindings.empty())
    {
        DasResult hr =
            BuildInputPortMap(frame, input_bindings, input_portmap.Put());
        if (DAS_S_OK != hr)
        {
            DAS_CORE_LOG_ERROR(
                "BuildInputPortMap failed for node = {}: hr = {}",
                node_id,
                static_cast<int>(hr));
            return hr;
        }
    }
    else
    {
        // No input bindings — create empty PortMap
        DasResult hr = CreateIDasPortMap(input_portmap.Put());
        if (DAS_S_OK != hr)
            return hr;
    }

    // 3. Call the component resolver (handles component lookup + execution)
    DAS::DasPtr<IDasPortMap> output_portmap;
    DasResult                hr = resolve_component(
        component_guid,
        p_stop_token,
        input_portmap.Get(),
        output_portmap.Put());
    if (DAS_S_OK != hr)
    {
        DAS_CORE_LOG_ERROR(
            "Component resolver failed for guid = {}, node = {}: hr = {}",
            component_guid,
            node_id,
            static_cast<int>(hr));
        return hr;
    }

    // 4. Extract output PortMap back to PortFrame for downstream nodes
    if (output_portmap.Get())
    {
        auto guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(node_id);
        hr = ExtractOutputPortMap(output_portmap.Get(), guid, frame);
        if (DAS_S_OK != hr)
        {
            DAS_CORE_LOG_ERROR(
                "ExtractOutputPortMap failed for node = {}: hr = {}",
                node_id,
                static_cast<int>(hr));
            return hr;
        }
    }

    return DAS_S_OK;
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------
DasResult GraphRuntime::Run(
    const Dto::CompiledGraphPlanDto&     plan,
    const std::string&                   current_fingerprint,
    Das::PluginInterface::IDasStopToken* p_stop_token,
    ComponentResolver                    resolve_component)
{
    last_error_.clear();

    // Phase 1: Fingerprint validation
    DasResult hr =
        ValidateFingerprint(plan.source_fingerprint, current_fingerprint);
    if (DAS_S_OK != hr)
    {
        last_error_ = DAS_FMT_NS::format(
            "Stale compiled artifact: source_fingerprint '{}' != current '{}'. "
            "Recompile the graph before execution.",
            plan.source_fingerprint,
            current_fingerprint);
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return hr;
    }

    // Phase 2: Build RuntimeExecutionCache from plan
    RuntimeExecutionCache cache;
    cache.BuildFrom(plan);

    if (!cache.IsBuilt())
    {
        last_error_ = "RuntimeExecutionCache::BuildFrom failed";
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return DAS_E_FAIL;
    }

    // Phase 3: Create fresh PortFrame for this execution
    PortFrame frame;

    // Phase 4: Per-node sequential execution (execution_order)
    for (const auto& node_id : plan.execution_order)
    {
        DAS_CORE_LOG_INFO("Running node = {}", node_id);

        // 4a: Check stop token before each node
        hr = CheckStopToken(p_stop_token);
        if (DAS_S_OK != hr)
        {
            last_error_ = DAS_FMT_NS::format(
                "Execution cancelled before node = {}",
                node_id);
            return hr;
        }

        // 4b: Find node snapshot in plan
        auto snapshot_it = std::find_if(
            plan.node_snapshots.begin(),
            plan.node_snapshots.end(),
            [&node_id](const Dto::CompiledNodeSnapshotDto& snap)
            { return snap.node_id == node_id; });

        if (snapshot_it == plan.node_snapshots.end())
        {
            last_error_ = DAS_FMT_NS::format(
                "Node '{}' not found in plan.node_snapshots",
                node_id);
            DAS_CORE_LOG_ERROR("{}", last_error_);
            return DAS_E_NOT_FOUND;
        }

        // 4c: Execute the node
        hr = ExecuteNode(
            node_id,
            snapshot_it->component_guid,
            p_stop_token,
            resolve_component,
            frame,
            plan);

        if (DAS_S_OK != hr)
        {
            last_error_ = DAS_FMT_NS::format(
                "Node '{}' execution failed: hr = {}",
                node_id,
                static_cast<int>(hr));
            DAS_CORE_LOG_ERROR("{}", last_error_);
            return hr;
        }

        DAS_CORE_LOG_TRACE("Node = {} completed successfully", node_id);
    }

    // Phase 5: Check stop token one final time (post-loop)
    hr = CheckStopToken(p_stop_token);
    if (DAS_S_OK != hr)
    {
        last_error_ = "Execution cancelled after completing all nodes";
        return hr;
    }

    DAS_CORE_LOG_INFO(
        "Execution complete: {} nodes processed",
        plan.execution_order.size());

    return DAS_S_OK;
}

DAS_CORE_GRAPHRUNTIME_NS_END
