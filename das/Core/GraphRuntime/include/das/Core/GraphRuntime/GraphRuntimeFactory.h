#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIMEFACTORY_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIMEFACTORY_H

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasGraphRuntime.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSession.Implements.hpp>

#include <string>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// Internal C++ implementation of IDasGraphRuntime COM interface.
// Wraps the engine-level GraphRuntime class behind a COM-compatible facade.
// Inherits from autogen ImplBase for automatic AddRef/Release/QueryInterface.
class GraphRuntimeImpl final
    : public Das::ExportInterface::DasGraphRuntimeImplBase<GraphRuntimeImpl>
{
    GraphRuntime                                           engine_;
    std::string                                            last_error_;
    DasPtr<Das::PluginInterface::IDasTaskComponentCreator> host_;

public:
    explicit GraphRuntimeImpl(
        Das::PluginInterface::IDasTaskComponentCreator* p_host);

    // --- IDasGraphRuntime interface ---
    DAS_IMPL GetErrorMessage(IDasReadOnlyString** pp_out_error_message);

    DAS_IMPL Execute(
        IDasReadOnlyString*                  p_compiled_artifact_json,
        Das::PluginInterface::IDasStopToken* p_stop_token,
        Das::ExportInterface::IDasJson**     pp_out_result_json);

    DAS_IMPL ExecuteWithTypedIO(
        IDasReadOnlyString*                        p_compiled_artifact_json,
        Das::PluginInterface::IDasStopToken*       p_stop_token,
        Das::ExportInterface::IDasReadOnlyPortMap* p_graph_inputs,
        Das::ExportInterface::IDasPortMap**        pp_out_outputs);

    DAS_IMPL GetLastExecutedNodeId(IDasReadOnlyString** pp_out_node_id);

private:
    // Shared artifact ingestion for the Execute entry points: read the
    // {fingerprint, plan} shell text, verify the plan payload against the
    // shell fingerprint (integrity gate), and cast it into @p plan. Writes
    // last_error_ on failure.
    DasResult ParseVerifiedPlan(
        IDasReadOnlyString*        p_compiled_artifact_json,
        Dto::CompiledGraphPlanDto& plan);
};

DAS_CORE_GRAPHRUNTIME_NS_END

// C ABI factory — primary declaration is auto-generated in
// DasCoreApi.generated.h.  This forward declaration ensures C++ code can
// call it before CMake reconfigure.
DAS_C_API DasResult
CreateGraphRuntime(Das::ExportInterface::IDasGraphRuntime** pp_out_runtime);

DAS_C_API DasResult CreateGraphRuntimeWithHost(
    Das::PluginInterface::IDasTaskComponentCreator* p_host,
    Das::ExportInterface::IDasGraphRuntime**        pp_out_runtime);

// ---------------------------------------------------------------------------
// Stateful authoring session C ABI (DAS-77)
// ---------------------------------------------------------------------------
// The authoritative store (GraphDocumentDto) lives in Core behind an opaque
// handle; plugins (which only link the stable C ABI) hold the handle and call
// these per GetDocument / ApplyChange / Compile. The store's public projection
// is the schema-contract authoring doc (AuthoringDocContract) —
// GraphDocumentDto never crosses the boundary.
//
// Inputs are UTF-8 JSON strings (IDasReadOnlyString); outputs are IDasJson.
// ---------------------------------------------------------------------------

/// Opaque Core-owned authoring session state (holds the GraphDocumentDto).
struct GraphAuthoringSessionState;

/// Create a session seeded from a context JSON. The context may be:
///   - a formSequence document ({ "items": [...] }) → projected to a linear
///     GraphDocumentDto and tagged "formsequence";
///   - a graph document ({ "nodes": [...], "edges": [...] }) → parsed directly;
///   - anything else / null → an empty (graph-mode) store.
///
/// p_creator 为 creator（TaskComponentCreator）借用指针（照
/// CreateGraphRuntimeWithHost(p_host) 先例，与执行侧完全对称，D-07）：
/// 会话编译路径经它开被引条目的 authoring 会话。nullptr = 不携带（现状
/// 行为）。借用生命周期照 node_faces_ 惯例——插件包持有的 host_ 保证
/// 本尊长命于会话，会话只借用不拥有。
DAS_C_API DasResult CreateGraphAuthoringSession(
    Das::PluginInterface::IDasTaskComponentCreator* p_creator,
    IDasReadOnlyString*                             p_context_json,
    GraphAuthoringSessionState**                    pp_out);

/// 测试缝：会话 state 是否携带 creator 借用指针（生产面零消费——creator
/// 携带通道的单测断言用；照 SetRepositoryEntryDocumentFetcherForTest 先例）。
bool GraphAuthoringSessionHasCreatorForTest(
    const GraphAuthoringSessionState* p);

/// Destroy a session and release its store. Safe on null.
DAS_C_API void DestroyGraphAuthoringSession(GraphAuthoringSessionState* p);

/// GetDocument: emit the schema-contract authoring doc for the current store
/// (formSequence view when linear-tagged, graph view otherwise). The runtime
/// GraphDocumentDto never leaks.
DAS_C_API DasResult GraphAuthoringSessionGetDocument(
    GraphAuthoringSessionState*      p,
    IDasReadOnlyString*              p_request_json,
    Das::ExportInterface::IDasJson** pp_out_document_json);

/// ApplyChange: parse a contract change JSON, dispatch the dual-state
/// ApplyAuthoringChange on the store, and return an authoring-result JSON
/// ({ "ok":bool, "revision":int, "errorKind"?:string, "message"?:string }).
DAS_C_API DasResult GraphAuthoringSessionApplyChange(
    GraphAuthoringSessionState*      p,
    IDasReadOnlyString*              p_change_json,
    Das::ExportInterface::IDasJson** pp_out_result_json);

/// Compile: compile the store into a CompiledGraphPlanDto JSON — the execution
/// artifact consumed by DasGraphTaskImpl::Do() / IDasGraphRuntime::Execute().
/// Port validation is skipped at the authoring boundary (no factory manager
/// bound); acceptable for authoring preview.
DAS_C_API DasResult GraphAuthoringSessionCompile(
    GraphAuthoringSessionState*      p,
    IDasReadOnlyString*              p_request_json,
    Das::ExportInterface::IDasJson** pp_out_compiled_plan_json);

/// Upgrade the store from formSequence (linear) to graph (canvas) view —
/// lossless (only the linear tag is removed). Returns DAS_E_FAIL if the store
/// is not in linear mode.
DAS_C_API DasResult
GraphAuthoringSessionUpgradeToGraph(GraphAuthoringSessionState* p);

/// Derive the boundary port surface of a graph document (ports are derived,
/// never authored): dangling input ports (no incoming edge, no same-name
/// explicit graph_input) project as boundary inputs, dangling output ports as
/// boundary outputs. `p_document_json` is a GraphDocumentDto JSON. The
/// declared-port-surface channel reads the session state's action catalog
/// (`p->action_entries`, seeded from the context `actionCatalog` key by
/// SeedFromContext) — the catalog's per-entry declared port surface resolves
/// manifest-declared ports for components referenced by the document.
/// Output is {inputs:[...],outputs:[...]} with each entry carrying
/// {portId,displayLabel,portType,isRequired,defaultValue,nodeIds}. The pure
/// derivation lives in Core; plugins (which cannot call Core C++ symbols
/// across the DLL boundary) reach it through this C ABI.
DAS_C_API DasResult GraphDeriveBoundaryPorts(
    GraphAuthoringSessionState* p,
    IDasReadOnlyString*         p_document_json,
    IDasReadOnlyString**        pp_out_boundary_json);

// ---------------------------------------------------------------------------
// Boundary component (graph begin/end) authoring session create export
// ---------------------------------------------------------------------------
// begin/end are GraphRuntime-internal structural components with their own
// authoring session (homologous to the plugin-provided MaaPI session): the
// state domain is the node's settings (boundaryPorts key), the document is a
// pure projection (formSequence items = write channel, root ports section =
// read channel), and the port direction is role-fixed (begin = input only,
// end = output only). The session class lives in
// GraphBoundaryAuthoringSession.{h,cpp}; this create entry is its C ABI
// export.
// ---------------------------------------------------------------------------

// C ABI factory — primary declaration is auto-generated in
// DasCoreApi.generated.h (the [ export, c_abi ] entry in
// idl/DasCoreApi.idl). This forward declaration ensures C++ code can call it
// before CMake reconfigure (same build-shim convention as CreateGraphRuntime
// above).
DAS_C_API DasResult CreateGraphBoundaryAuthoringSession(
    const DasGuid*                                   p_component_guid,
    IDasReadOnlyString*                              p_context_json,
    Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session);

// ---------------------------------------------------------------------------
// Component bindings scan / node document write-back (scheduler
// orchestration support)
// ---------------------------------------------------------------------------
// Graph-document shape knowledge (componentRef extraction, taskRepositoryRef
// binding judgment, node settings read/replace, removed-port edge cascade)
// lives in GraphRuntime; the scheduler (authoring faces pre-pass, boundary
// document pipeline) orchestrates on the business-neutral binding list these
// entries emit. Primary declarations are auto-generated in
// DasCoreApi.generated.h; these forward declarations follow the same
// build-shim convention as CreateGraphBoundaryAuthoringSession above.
// ---------------------------------------------------------------------------

// Scan a graph document (raw GraphDocumentDto shape, top-level nodes array;
// non-graph documents yield an empty array) for componentRef nodes:
// [{nodeId, componentGuid, boundEntryId?, settings}]. boundEntryId is emitted
// only when the action catalog declares a taskRepositoryRef config field for
// the component and the node settings carry a positive integer under it.
DAS_C_API DasResult GraphScanComponentBindings(
    IDasReadOnlyString*              p_graph_document_json,
    IDasReadOnlyString*              p_action_catalog_json,
    Das::ExportInterface::IDasJson** pp_out_bindings);

// Replace the node's settings wholesale and cascade-delete edges referencing
// the node's removed ports (both sides — same erase semantics as the
// removeNodePort apply path). Emits the updated full graph document (other
// top-level keys preserved verbatim; an unknown node yields a verbatim copy).
DAS_C_API DasResult GraphWriteComponentNodeDocument(
    IDasReadOnlyString*              p_graph_document_json,
    IDasReadOnlyString*              p_node_id,
    IDasReadOnlyString*              p_node_settings_json,
    IDasReadOnlyString*              p_removed_port_ids_json,
    Das::ExportInterface::IDasJson** pp_out_document);

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIMEFACTORY_H
