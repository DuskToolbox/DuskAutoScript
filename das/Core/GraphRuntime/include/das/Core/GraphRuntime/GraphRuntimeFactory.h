#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIMEFACTORY_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIMEFACTORY_H

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasGraphRuntime.Implements.hpp>

#include <string>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// Internal C++ implementation of IDasGraphRuntime COM interface.
// Wraps the engine-level GraphRuntime class behind a COM-compatible facade.
// Inherits from autogen ImplBase for automatic AddRef/Release/QueryInterface.
class GraphRuntimeImpl final
    : public Das::ExportInterface::DasGraphRuntimeImplBase<GraphRuntimeImpl>
{
    GraphRuntime                                        engine_;
    std::string                                         last_error_;
    DasPtr<Das::PluginInterface::IDasTaskComponentHost> host_;

public:
    explicit GraphRuntimeImpl(
        Das::PluginInterface::IDasTaskComponentHost* p_host);

    // --- IDasGraphRuntime interface ---
    DAS_IMPL GetErrorMessage(IDasReadOnlyString** pp_out_error_message);

    DAS_IMPL Execute(
        IDasReadOnlyString*                  p_compiled_artifact_json,
        Das::PluginInterface::IDasStopToken* p_stop_token,
        Das::ExportInterface::IDasJson**     pp_out_result_json);
};

DAS_CORE_GRAPHRUNTIME_NS_END

// C ABI factory — primary declaration is auto-generated in
// DasCoreApi.generated.h.  This forward declaration ensures C++ code can
// call it before CMake reconfigure.
DAS_C_API DasResult
CreateGraphRuntime(Das::ExportInterface::IDasGraphRuntime** pp_out_runtime);

DAS_C_API DasResult CreateGraphRuntimeWithHost(
    Das::PluginInterface::IDasTaskComponentHost* p_host,
    Das::ExportInterface::IDasGraphRuntime**     pp_out_runtime);

// ---------------------------------------------------------------------------
// Stateful authoring session C ABI (DAS-77)
// ---------------------------------------------------------------------------
// The authoritative store (GraphDocumentDto) lives in Core behind an opaque
// handle; plugins (which only link the stable C ABI) hold the handle and call
// these per GetDocument / ApplyChange / Compile. The store's public projection
// is the schema-contract authoring doc (AuthoringDocContract) — GraphDocumentDto
// never crosses the boundary.
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
DAS_C_API DasResult CreateGraphAuthoringSession(
    IDasReadOnlyString*             p_context_json,
    GraphAuthoringSessionState**    pp_out);

/// Destroy a session and release its store. Safe on null.
DAS_C_API void DestroyGraphAuthoringSession(GraphAuthoringSessionState* p);

/// GetDocument: emit the schema-contract authoring doc for the current store
/// (formSequence view when linear-tagged, graph view otherwise). The runtime
/// GraphDocumentDto never leaks.
DAS_C_API DasResult GraphAuthoringSessionGetDocument(
    GraphAuthoringSessionState*         p,
    IDasReadOnlyString*                 p_request_json,
    Das::ExportInterface::IDasJson**    pp_out_document_json);

/// ApplyChange: parse a contract change JSON, dispatch the dual-state
/// ApplyAuthoringChange on the store, and return an authoring-result JSON
/// ({ "ok":bool, "revision":int, "errorKind"?:string, "message"?:string }).
DAS_C_API DasResult GraphAuthoringSessionApplyChange(
    GraphAuthoringSessionState*         p,
    IDasReadOnlyString*                 p_change_json,
    Das::ExportInterface::IDasJson**    pp_out_result_json);

/// Compile: compile the store into a CompiledGraphPlanDto JSON — the execution
/// artifact consumed by DasGraphTaskImpl::Do() / IDasGraphRuntime::Execute().
/// Port validation is skipped at the authoring boundary (no factory manager
/// bound); acceptable for authoring preview.
DAS_C_API DasResult GraphAuthoringSessionCompile(
    GraphAuthoringSessionState*         p,
    IDasReadOnlyString*                 p_request_json,
    Das::ExportInterface::IDasJson**    pp_out_compiled_plan_json);

/// Upgrade the store from formSequence (linear) to graph (canvas) view —
/// lossless (only the linear tag is removed). Returns DAS_E_FAIL if the store
/// is not in linear mode.
DAS_C_API DasResult GraphAuthoringSessionUpgradeToGraph(
    GraphAuthoringSessionState* p);

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIMEFACTORY_H
