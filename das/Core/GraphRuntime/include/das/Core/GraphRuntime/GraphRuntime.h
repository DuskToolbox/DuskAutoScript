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

#include <string>
#include <unordered_map>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// Forward declarations
class PortFrame;

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

    // Get error message from last failed execution.
    const std::string& GetLastErrorMessage() const { return last_error_; }

    // 最近一次运行实际最后 Done 的节点 id（运行期事实；空串=没跑过任何
    // 节点）。区别于计划 execution_order 末位——尾部节点被信号门跳过时
    // 末位节点从未执行，本值不受影响。
    const std::string& GetLastExecutedNodeId() const
    {
        return last_executed_node_id_;
    }

    // 最近一次成功执行的边界输出口值映射（{port_id: value}，按编译产物
    // boundary_ports.outputs 清单从帧导出；无清单/无值时为空对象）。frame
    // 只在 RunWithHost 作用域内存活，导出在引擎侧完成后经此暴露给 COM
    // 包装层组装结果 JSON。
    const yyjson::value& GetLastGraphOutputs() const
    {
        return last_graph_outputs_;
    }

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
    //
    // @p pp_out_typed_outputs, when non-null, receives a freshly created
    // IDasPortMap holding the boundary output values exported as typed
    // entries (object references included) — the typed counterpart of the
    // JSON graphOutputs channel. Null keeps the legacy three-parameter
    // behaviour untouched.
    DasResult RunWithHost(
        const Dto::CompiledGraphPlanDto&             plan,
        const std::string&                           current_fingerprint,
        Das::PluginInterface::IDasStopToken*         p_stop_token,
        Das::PluginInterface::IDasTaskComponentHost* p_host,
        Das::ExportInterface::IDasPortMap** pp_out_typed_outputs = nullptr);

    // Typed graph-level inputs consulted by the broadcast-binding overlay
    // during execution (keys are boundary port ids). Non-owning: the caller
    // (ExecuteWithTypedIO) keeps the PortMap alive across the call and
    // clears the member on return. Null keeps the default-only behaviour.
    void SetGraphInputs(Das::ExportInterface::IDasReadOnlyPortMap* graph_inputs)
    {
        graph_inputs_ = graph_inputs;
    }

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

    // 运行期记录：最近一次实际最后 Done 的节点（RunWithHost 开头清空）。
    std::string last_executed_node_id_;

    // 最近一次成功执行的 graphOutputs 导出（成员形式暴露给包装层）。
    yyjson::value last_graph_outputs_;

    // Per-node component cache populated by Configure().
    std::unordered_map<std::string, NodeComponentEntry> node_components_;

    // True after Configure() has been called at least once.
    bool configured_ = false;

    // Typed graph-level inputs for the current run (non-owning; see
    // SetGraphInputs).
    Das::ExportInterface::IDasReadOnlyPortMap* graph_inputs_ = nullptr;

    // Optional error lens for structured error propagation.
    Das::PluginInterface::IDasErrorLens* p_error_lens_ = nullptr;

    // Validate compiled plan fingerprint against current graph fingerprint.
    // Returns DAS_S_OK on match, DAS_E_FAIL on mismatch.
    static DasResult ValidateFingerprint(
        const std::string& compiled_source_fingerprint,
        const std::string& current_fingerprint);

    // Execute a node using its pre-configured IDasTaskComponent.
    // Converts PortMap ↔ JSON for the IDasTaskComponent::Do() ABI.
    DasResult ExecuteNodeWithComponent(
        const std::string&                   node_id,
        Das::PluginInterface::IDasStopToken* p_stop_token,
        PortFrame&                           frame,
        const Dto::CompiledGraphPlanDto&     plan);

    // Signal-driven ready-queue scheduler (DAS-60 Stage 3). Activates a node
    // when its data predecessors are resolved AND its signal gate is open,
    // propagates skip along data edges, and re-activates loop heads on
    // back-edge firing. Used only when the plan carries signal routes or
    // back edges; pure-data graphs keep the linear execution_order path.
    DasResult RunSignalGated(
        const Dto::CompiledGraphPlanDto&     plan,
        Das::PluginInterface::IDasStopToken* p_stop_token,
        PortFrame&                           frame);

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
