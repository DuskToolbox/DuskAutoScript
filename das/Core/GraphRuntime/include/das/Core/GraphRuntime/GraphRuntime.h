#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIME_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIME_H

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/DasPtr.hpp>
#include <das/DasTypes.hpp>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasTask.h>

#include <functional>
#include <string>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// Forward declarations
class PortFrame;

// ---------------------------------------------------------------------------
// ComponentResolver — combines component lookup + execution into one callback
//
// Bridges the gap between GraphRuntime's PortMap-based internal data plane
// and the actual IDasTaskComponent::Do() JSON-based ABI.
//
// The resolver receives: component_guid, stop_token, input PortMap, and
// must produce an output PortMap via pp_out_map.
//
// In production (Plan 02-14), this bridges to IDasTaskComponent::Do() with
// JSON↔PortMap conversion. In tests, simple lambdas provide mock behavior.
// ---------------------------------------------------------------------------
using ComponentResolver = std::function<DasResult(
    const std::string&                   component_guid,
    Das::PluginInterface::IDasStopToken* p_stop_token,
    Das::ExportInterface::IDasPortMap*   input_map,
    Das::ExportInterface::IDasPortMap**  pp_out_map)>;

// ---------------------------------------------------------------------------
// GraphRuntime — Execution engine core
//
// Loads CompiledGraphPlanDto, validates fingerprint staleness, creates
// per-run PortFrame + RuntimeExecutionCache, and executes nodes sequentially
// in execution_order via DoAdapter (PortMap data plane).
// ---------------------------------------------------------------------------
class GraphRuntime
{
public:
    GraphRuntime() = default;
    ~GraphRuntime() = default;

    // Run a compiled graph plan.
    //
    // plan:                Pre-compiled CompiledGraphPlanDto
    // current_fingerprint: Graph document's current fingerprint for staleness
    // p_stop_token:        Caller-provided stop token (null = never cancelled)
    // resolve_component:   Callback combining component lookup + execution
    //
    // Returns DAS_S_OK on success; error codes on failure.
    DasResult Run(
        const Dto::CompiledGraphPlanDto&     plan,
        const std::string&                   current_fingerprint,
        Das::PluginInterface::IDasStopToken* p_stop_token,
        ComponentResolver                    resolve_component);

    // Validate compiled plan fingerprint against current graph fingerprint.
    // Returns DAS_S_OK on match, DAS_E_FAIL on mismatch.
    static DasResult ValidateFingerprint(
        const std::string& compiled_source_fingerprint,
        const std::string& current_fingerprint);

    // Get error message from last failed execution.
    const std::string& GetLastErrorMessage() const { return last_error_; }

private:
    std::string last_error_;

    // Per-node execution: build input PortMap, call resolver, extract output.
    DasResult ExecuteNode(
        const std::string&                   node_id,
        const std::string&                   component_guid,
        Das::PluginInterface::IDasStopToken* p_stop_token,
        ComponentResolver&                   resolve_component,
        PortFrame&                           frame,
        const Dto::CompiledGraphPlanDto&     plan);

    // Check stop token; returns DAS_E_FAIL if cancelled.
    static DasResult CheckStopToken(
        Das::PluginInterface::IDasStopToken* p_stop_token);
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIME_H
