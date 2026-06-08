#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIME_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIME_H

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/DasPtr.hpp>
#include <das/DasTypes.hpp>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>

#include <functional>
#include <string>
#include <unordered_map>

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
// NodeComponentEntry — per-node execution context created by Configure()
//
// Holds the IDasTaskComponent instance created from IDasTaskComponentHost,
// along with metadata about whether settings have been applied (v17 data-sep).
// ---------------------------------------------------------------------------
struct NodeComponentEntry
{
    std::string                                          node_id;
    std::string                                          component_guid;
    DAS::DasPtr<Das::PluginInterface::IDasTaskComponent> component;
    bool settings_applied = false;
};

// ---------------------------------------------------------------------------
// GraphRuntime — Execution engine core
//
// Loads CompiledGraphPlanDto, validates fingerprint staleness, creates
// per-run PortFrame + RuntimeExecutionCache, and executes nodes sequentially
// in execution_order via DoAdapter (PortMap data plane).
//
// Plan 02-14 adds Configure/Prepare/RunWithHost:
//   - Configure() creates per-node IDasTaskComponent instances from
//     IDasTaskComponentHost and applies compiled_settings/compiled_payload_json
//     via ApplySettingsChange (v17 data-sep: settings bound pre-Do, NOT in
//     PortMap).
//   - Prepare() validates all configured components are ready.
//   - RunWithHost() is the full Configure → Prepare → Execute pipeline.
//   - IDasErrorLens integration provides structured error messages.
// ---------------------------------------------------------------------------
class GraphRuntime
{
public:
    GraphRuntime() = default;
    ~GraphRuntime() = default;

    // -- Legacy Run API (Plan 02-13) ----------------------------------------

    // Run a compiled graph plan using a ComponentResolver callback.
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

    // -- Configure / Prepare / RunWithHost (Plan 02-14) ---------------------

    // Configure: create per-node IDasTaskComponent instances from
    // IDasTaskComponentHost, then apply compiled_settings and
    // compiled_payload_json via ApplySettingsChange.
    //
    // v17 data-sep: settings and compiledPayload are bound here, pre-Do,
    // NOT passed through the PortMap data plane.
    //
    // Must be called before Prepare() and RunWithHost().
    DasResult Configure(
        const Dto::CompiledGraphPlanDto&             plan,
        Das::PluginInterface::IDasTaskComponentHost* p_host);

    // Prepare: validate all configured components are ready for execution.
    // Must be called after Configure().
    DasResult Prepare() const;

    // RunWithHost: full Configure → Prepare → Execute pipeline using
    // IDasTaskComponentHost for real component resolution.
    //
    // Internally calls Configure(), Prepare(), then executes nodes
    // using the pre-configured IDasTaskComponent instances.
    DasResult RunWithHost(
        const Dto::CompiledGraphPlanDto&             plan,
        const std::string&                           current_fingerprint,
        Das::PluginInterface::IDasStopToken*         p_stop_token,
        Das::PluginInterface::IDasTaskComponentHost* p_host);

    // Set an IDasErrorLens for structured error message lookup.
    void SetErrorLens(Das::PluginInterface::IDasErrorLens* p_lens);

    // Query a structured error message from the configured IDasErrorLens.
    // Returns DAS_S_OK if a message was found, DAS_E_NOT_FOUND otherwise.
    DasResult GetStructuredError(DasResult error_code, std::string& out_message)
        const;

    // Clear all per-node component state (e.g., between runs).
    void ResetNodeComponents();

private:
    std::string last_error_;

    // Per-node component cache populated by Configure().
    std::unordered_map<std::string, NodeComponentEntry> node_components_;

    // Optional error lens for structured error propagation.
    Das::PluginInterface::IDasErrorLens* p_error_lens_ = nullptr;

    // Per-node execution: build input PortMap, call resolver, extract output.
    DasResult ExecuteNode(
        const std::string&                   node_id,
        const std::string&                   component_guid,
        Das::PluginInterface::IDasStopToken* p_stop_token,
        ComponentResolver&                   resolve_component,
        PortFrame&                           frame,
        const Dto::CompiledGraphPlanDto&     plan);

    // Execute a node using its pre-configured IDasTaskComponent.
    // Converts PortMap ↔ JSON for the IDasTaskComponent::Do() ABI.
    DasResult ExecuteNodeWithComponent(
        const std::string&                   node_id,
        Das::PluginInterface::IDasStopToken* p_stop_token,
        PortFrame&                           frame,
        const Dto::CompiledGraphPlanDto&     plan);

    // Check stop token; returns DAS_E_FAIL if cancelled.
    static DasResult CheckStopToken(
        Das::PluginInterface::IDasStopToken* p_stop_token);

    // Apply compiled_settings + compiled_payload_json to a component via
    // ApplySettingsChange.  Returns DAS_S_OK on success.
    DasResult ApplyNodeSettings(
        const Dto::CompiledNodeSnapshotDto&      snapshot,
        Das::PluginInterface::IDasTaskComponent* p_component);

    // Set last_error_ and optionally query IDasErrorLens for detail.
    void SetError(DasResult error_code, const std::string& message);
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIME_H
