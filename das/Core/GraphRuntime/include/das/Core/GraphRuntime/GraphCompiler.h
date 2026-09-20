#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHCOMPILER_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHCOMPILER_H

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphDocument.h>

#include <array>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declaration — avoid pulling full ForeignInterfaceHost header.
DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN
class TaskComponentFactoryManager;
DAS_CORE_FOREIGNINTERFACEHOST_NS_END

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// Import forward-declared type into this namespace for convenience.
using ForeignInterfaceHost::TaskComponentFactoryManager;

// ---------------------------------------------------------------------------
// Diagnostic types for compile-time edge validation
// ---------------------------------------------------------------------------

enum class CompileDiagnosticKind
{
    PortNotFound,              // Edge references a port_id not in the manifest
    TypeMismatch,              // Source port type incompatible with target
    NodeNotFound,              // Edge references a node_id not in the graph
    UnknownPortType,           // Port type string not in known set (warning)
    UnresolvableComponentGuid, // component_guid not in factory manager
    CyclicEdgeGraph,      // Edge-based cycle detected during topological sort
    RequiredFieldMissing, // Required field has no usable value in node settings
    UnbackedDataCycle, // Data cycle without a signal circuit carrying the loop
    LoopBodyDataLeak,  // Loop-body data edge leaves the loop (stale-value risk)
    // loopCarried output port without a same-name input port to pair with
    LoopCarriedPortInvalid,
    // begin/end 数量约束是 session graph 视图契约——Core 编译器本体不检
    // 查（formSequence/maapi/缓存 plan 场景 begin 永远缺席），检查与诊断
    // 的产出都在 session compile 层（GraphAuthoringSessionCompile），此处
    // 只定义 kind 词汇供 wire 序列化消费。
    BeginNodeMissing,   // graph-view document carries no Begin node
    BeginNodeDuplicate, // graph-view document carries more than one Begin node
    EndNodeDuplicate,   // graph-view document carries more than one End node
    // 存储边经口面演化复核后标记断开：口的声明已随文档演化消失，或两
    // 端类型按当前口面不再兼容。诊断是 WARN 级（非阻断）——复核产物在
    // Apply 语境作为文档变更写回（edges 删除、revision 推进），纯读编
    // 译路径只携带诊断零修剪。
    StaleEdgeDisconnect
};

struct CompileEdgeDiagnostic
{
    CompileDiagnosticKind kind = CompileDiagnosticKind::PortNotFound;
    std::string           node_id; // Node this diagnostic refers to
    std::string           port_id; // Port (empty for node-level diagnostics)
    std::string           edge_id; // Edge (empty if not edge-specific)
    std::string           message; // Human-readable message (English)

    enum class PortDirection
    {
        Input,
        Output,
        Unknown
    };
    PortDirection direction = PortDirection::Unknown;

    // For TypeMismatch: expected (target input) vs actual (source output)
    std::string expected_type;
    std::string actual_type;
};

// ---------------------------------------------------------------------------
// GraphCompiler — compile-layer service for manifest reading + edge validation
// ---------------------------------------------------------------------------

class GraphCompiler
{
public:
    GraphCompiler();

    /// Set the factory manager for manifest resolution. Must be called before
    /// ValidateEdgePorts() or ReadManifest().
    void SetFactoryManager(TaskComponentFactoryManager* factory_manager);

    // -----------------------------------------------------------------------
    // Port entry types returned by ReadManifest
    // -----------------------------------------------------------------------
    struct PortEntry
    {
        std::string port_id;
        std::string port_type;
        // Mirrors GraphPortDefinitionDto::loop_carried (manifest channel).
        bool loop_carried = false;
    };

    struct ManifestPorts
    {
        std::vector<PortEntry> inputs;
        std::vector<PortEntry> outputs;
        bool                   was_resolved = false;
    };

    /// Read manifest definition.inputs/outputs for a given component_guid.
    /// Returns {inputs, outputs} port lists. Empty vectors if not found.
    ManifestPorts ReadManifest(const std::string& component_guid) const;

    /// (source_node_id, source_port_id, target_node_id, target_port_id) —
    /// the coordinate system signal routes are matched by.
    using SignalEdgeCoordinate = std::array<std::string, 4>;

    /// Validate all edges in a GraphDocumentDto against node manifests.
    /// For each edge:
    ///   1. Verify source_node_id and target_node_id exist in graph
    ///   2. Resolve source node manifest -> check source_port_id in outputs
    ///   3. Resolve target node manifest -> check target_port_id in inputs
    ///   4. Check type compatibility between source output and target input
    /// Returns: vector of diagnostics (empty if all edges valid).
    ///
    /// When @p canonical_signal_edges is non-null it collects the coordinates
    /// of every canonical signal source edge (edge_type "signal" leaving the
    /// canonical signal_out port, undeclared or declared as signal type) —
    /// the manifest knowledge only this member path holds; Compile marks the
    /// matching signal routes so the runtime can backfill their emission.
    std::vector<CompileEdgeDiagnostic> ValidateEdgePorts(
        const Dto::GraphDocumentDto&    document,
        std::set<SignalEdgeCoordinate>* canonical_signal_edges = nullptr);

    /// Compute topological order of nodes based on edge dependencies.
    ///
    /// Signal-aware (DAS-60): data edges and forward signal edges participate
    /// in Kahn's algorithm; signal back-edges (a signal edge returning to a
    /// topologically-earlier node, i.e. a loop head) are tolerated and excluded
    /// from ordering. Only a cycle among **data** edges yields an empty result.
    /// Ordering is deterministic: ties are broken by node order in the document
    /// so signal chains/sequence graphs always produce a stable
    /// execution_order.
    std::vector<std::string> ComputeExecutionOrder(
        const Dto::GraphDocumentDto& document);

    /// Generate port binding plan from edges + manifest port type info.
    /// Each edge becomes one PortBindingDto with expected_type from source.
    /// Dangling input ports additionally derive $graph_input broadcast
    /// bindings (the graph's boundary inputs).
    Dto::PortBindingPlanDto GeneratePortBindingPlan(
        const Dto::GraphDocumentDto& document);

    // -----------------------------------------------------------------------
    // Port surface provider (session-catalog channel)
    // -----------------------------------------------------------------------

    /// Registry/catalog-declared port surface of one component. The
    /// declaration carries the direction (inputs/outputs list membership);
    /// the document itself stores no port declarations.
    struct ComponentPortSurface
    {
        std::vector<Dto::GraphPortDefinitionDto> inputs;
        std::vector<Dto::GraphPortDefinitionDto> outputs;
    };

    /// component_guid → declared port surface (nullopt = unknown component).
    /// The authoring-session compile path has no factory manager bound, so
    /// the session layer derives the provider from its action catalog and
    /// injects it here — the same injected-knowledge pattern as the
    /// required_fields map (the compiler has no catalog access).
    using ComponentPortSurfaceProvider =
        std::function<std::optional<ComponentPortSurface>(
            const std::string& component_guid)>;

    // -----------------------------------------------------------------------
    // Document port faces (nodeFaces / entryFaces context channel)
    // -----------------------------------------------------------------------

    /// One dynamic port entry of the faces channel — the authoring-document
    /// port snapshot riding the session context as a data payload (compile
    /// host performs no live queries). Wire shape per port:
    /// {id, type, direction[, label]} — label defaults to the id when absent;
    /// direction is carried per entry and merges single-sided.
    struct DocumentFacePort
    {
        std::string port_id;
        std::string port_type;
        std::string label;
        std::string direction; // "input" | "output"
    };

    /// node_id → injected dynamic port faces. Keyed by node: two nodes
    /// sharing one component_guid never share faces — the merge is per node,
    /// never a guid aggregation.
    using NodeFacesMap =
        std::unordered_map<std::string, std::vector<DocumentFacePort>>;

    /// One registered custom type entry of the faces channel's optional
    /// types[] array ({name[, compatibleWith]} shape). Parsed and stored
    /// alongside the faces as pass-through data; consumption (the
    /// projection-side type registry) lands with the partition retirement
    /// plan.
    struct DocumentFaceType
    {
        std::string              name;
        std::vector<std::string> compatible_with;
    };

    /// entry_id (decimal string of the positive integer the node settings
    /// carry) → injected document port faces of that repository entry (the
    /// invoked document's dynamic port snapshot).
    using EntryFacesMap =
        std::unordered_map<std::string, std::vector<DocumentFacePort>>;

    /// component_guid → names of its taskRepositoryRef-typed config fields
    /// (catalog-derived data snapshot injected by the session compile path —
    /// the same injected-knowledge pattern as required_fields; the compiler
    /// never queries the catalog).
    using RepositoryFieldMap =
        std::unordered_map<std::string, std::unordered_set<std::string>>;

    /// Bind the injected per-node document port faces. Extends (does not
    /// replace) the declared-surface resolution: ResolveNodePorts merges the
    /// node's faces on top of the manifest/catalog-declared surface, dedupes
    /// by port id with the face winning over a same-name declared port, and
    /// WARNs on the collision (the document side wins; the surface never
    /// carries a duplicate port id). The map is borrowed for the duration of
    /// the synchronous Compile call — the session state outlives it (the
    /// same lifecycle premise as the port-surface provider closure).
    void BindNodeFaces(const NodeFacesMap* node_faces);

    /// Bind the injected entry faces plus the repository-field snapshot.
    /// ResolveNodePorts merges entryFaces[entryId] onto nodes holding a
    /// bound repository entry (a taskRepositoryRef field carrying a positive
    /// integer in settings — the business-neutral bound-entry judgment),
    /// after the node's own faces and with the same dedupe semantics (face
    /// wins, WARN on collision). The entry-faces map is borrowed for the
    /// duration of the synchronous Compile call; the repository-field
    /// snapshot is copied.
    void BindEntryFaces(
        const EntryFacesMap* entry_faces,
        RepositoryFieldMap   repository_fields);

    /// Bind the catalog-declared port surface provider. Extends (does not
    /// replace) factory-manager manifest resolution: edge validation,
    /// binding-plan generation and boundary derivation all consult it when
    /// the manifest path is unbound or unresolved.
    void SetPortSurfaceProvider(ComponentPortSurfaceProvider provider);

    /// Bind the injected document type registry (the types[] array riding
    /// the entryFaces context key — the same registry source the graph-view
    /// projection exposes as its root types section). The registry extends
    /// the type judgment of the
    /// stale-edge review only: registered custom types become judgeable
    /// (compatibleWith declarations are the authority, undeclared pairs are
    /// incompatible), while the built-in table stays the single authority
    /// of IsTypeCompatible (no second equivalence table). Borrowed for the
    /// duration of the synchronous Compile call — the same lifecycle
    /// premise as BindNodeFaces.
    void BindFaceTypes(const std::vector<DocumentFaceType>* face_types);

    /// Check type compatibility between source output and target input.
    /// The Core-side authoritative half of the numeric-equivalence table —
    /// the UI-side GraphConnectionRules.CanConnect (DasNativeUI) mirrors
    /// the same rules; the two sides must not drift.
    static bool IsTypeCompatible(
        const std::string& source_type,
        const std::string& target_type);

    // -----------------------------------------------------------------------
    // Stored-edge stale review (compile-time auto-disconnect)
    // -----------------------------------------------------------------------

    /// One stored edge the review marked for disconnection. The reason
    /// mirrors the two auto-disconnect clauses: a port id gone from the
    /// node's current face (source/target side), or both endpoints resolved
    /// but the types no longer compatible under the injected registry.
    struct StaleEdgeRecord
    {
        enum class Reason
        {
            SourcePortMissing,
            TargetPortMissing,
            TypeIncompatible
        };

        std::string edge_id;
        std::string source_node_id;
        std::string source_port_id;
        std::string target_node_id;
        std::string target_port_id;
        Reason      reason = Reason::SourcePortMissing;
        // TypeIncompatible only: the two endpoint types as resolved against
        // the current faces (diagnostic locator fields).
        std::string source_type;
        std::string target_type;
    };

    /// Review product: the disconnect set plus the WARN diagnostics — the
    /// compile output contract the session Apply path consumes (Apply
    /// applies the set as an ordinary document change; the pure read
    /// compile paths carry the diagnostics only, zero mutation).
    struct StaleEdgeReview
    {
        std::vector<StaleEdgeRecord>       stale_edges;
        std::vector<CompileEdgeDiagnostic> diagnostics;
    };

    /// Re-check every stored data edge against the nodes' current port
    /// faces (declared surface ∪ injected faces — the per-node union).
    /// A data edge goes stale when a face-evolved endpoint lost the port id
    /// or the endpoint types stopped being compatible (IsTypeCompatible
    /// authority, extended by the bound document type registry). Every
    /// stale edge logs a WARN carrying both endpoint node/port ids and the
    /// reason. Signal edges are outside the review (they ride the canonical
    /// sequencing coordinate system, not the data-port face); nodes without
    /// an injected faces entry are outside it too — an absent entry means
    /// the compile host did not run the prep pass, not that the face
    /// evolved to empty, so pruning there would delete healthy edges on a
    /// transient context gap.
    StaleEdgeReview ReviewStaleEdges(
        const Dto::GraphDocumentDto& document) const;

    // -----------------------------------------------------------------------
    // Boundary port derivation (ports are derived, never authored)
    // -----------------------------------------------------------------------

    /// Derive the boundary port surface of a document by pure document
    /// analysis — no repository access. The two sides are asymmetric by
    /// design: an input port with no incoming edge projects as a boundary
    /// input (exposing an input is the "value needs a source" minimum set),
    /// while EVERY declared output port projects as a boundary output
    /// (internal consumption and external export are not mutually exclusive
    /// — fan-out is legal, so half-boundary ports survive extraction).
    /// Same-name sites merge under one boundary port (by-name broadcast
    /// semantics); the first occurrence contributes the port metadata.
    static Dto::BoundaryPortsDto DeriveBoundaryPorts(
        const Dto::GraphDocumentDto&        document,
        const ComponentPortSurfaceProvider& provider);

    /// Member form: resolves declared port surfaces via the bound factory
    /// manager / injected catalog provider, then runs the static derivation.
    Dto::BoundaryPortsDto DeriveBoundaryPorts(
        const Dto::GraphDocumentDto& document) const;

    /// Validate that every required field declared for a node's component
    /// has a usable value in the node settings. `required_fields` maps
    /// component_guid → required field-name set — the compiler has no
    /// catalog access, so the session layer derives the map from its action
    /// catalog (the manifest-declared required flags) and injects it. A
    /// field is empty when the settings key is absent or null; string values
    /// are additionally empty when blank (whitespace-only), matching the
    /// runtime launch exe_path.empty() judgement.
    static std::vector<CompileEdgeDiagnostic> ValidateRequiredFields(
        const Dto::GraphDocumentDto& document,
        const std::unordered_map<std::string, std::unordered_set<std::string>>&
            required_fields);

    /// Convert a CompileEdgeDiagnostic to a yyjson::value for plan
    /// diagnostics. Public so session-layer compile checks (begin/end node
    /// constraints) emit the exact same wire shape as the compiler's own
    /// diagnostics.
    static yyjson::value DiagnosticToJson(const CompileEdgeDiagnostic& diag);

    /// Project an internal plan diagnostic (kind/message shape) into the
    /// strict scheduler diagnostic contract ({severity, message, code}) for
    /// envelopes crossing the scheduler boundary. severity derives from the
    /// kind semantics: the two non-blocking lint families (UnknownPortType /
    /// LoopBodyDataLeak) surface as warnings, everything else is an error.
    static yyjson::value DiagnosticToWireContract(
        const yyjson::value& diagnostic);

    /// Unified compile entry point: runs all phases and produces a
    /// CompiledGraphPlanDto. Steps: ValidateEdgePorts ->
    /// ComputeExecutionOrder -> GeneratePortBindingPlan.
    Dto::CompiledGraphPlanDto Compile(const Dto::GraphDocumentDto& document);

    /// Required-aware overload: additionally runs ValidateRequiredFields and
    /// merges its diagnostics into the plan.
    Dto::CompiledGraphPlanDto Compile(
        const Dto::GraphDocumentDto& document,
        const std::unordered_map<std::string, std::unordered_set<std::string>>&
            required_fields);

    // -----------------------------------------------------------------------
    // Signal-aware topology analysis (DAS-60 Stage 2)
    // -----------------------------------------------------------------------

    /// Result of signal-aware topological analysis. Carries the deterministic
    /// execution_order plus the signal_routes (every signal edge), the
    /// back_edges (every loop-closing edge, signal or data — the loop-carried
    /// channels), the loop_layers (cycle SCCs expanded with signal-less
    /// producer closures; the runtime's per-head iteration reset sets) and
    /// the carrying-check diagnostics (a data cycle layer with no signal
    /// circuit — Compile() turns those into a rejected plan).
    struct TopologyResult
    {
        std::vector<std::string>           execution_order;
        std::vector<Dto::SignalRouteDto>   signal_routes;
        std::vector<Dto::BackEdgeDto>      back_edges;
        std::vector<Dto::LoopLayerDto>     loop_layers;
        std::vector<CompileEdgeDiagnostic> diagnostics;
    };

    /// DFS back-edge classification + Tarjan SCC layering + deterministic
    /// Kahn over (data edges ∪ forward edges with every back edge removed).
    /// Signal and data back-edges are both recorded (not ordered); data
    /// back-edges are loop-carried state channels and no longer reject the
    /// graph — carrying checks live one layer up in Compile().
    TopologyResult ComputeTopology(const Dto::GraphDocumentDto& document);

private:
    TaskComponentFactoryManager* factory_manager_ = nullptr;

    ComponentPortSurfaceProvider port_surface_provider_;

    // Borrowed per-node document faces (nullptr = no faces channel bound).
    const NodeFacesMap* node_faces_ = nullptr;

    // Borrowed entry faces (nullptr = no entry-faces channel bound).
    const EntryFacesMap* entry_faces_ = nullptr;

    // Borrowed document type registry (nullptr = no types channel bound).
    const std::vector<DocumentFaceType>* face_types_ = nullptr;

    // Catalog-derived snapshot: component_guid → taskRepositoryRef field
    // names (the bound-entry judgment's data source).
    RepositoryFieldMap repository_fields_;

    /// Resolve port entries from a manifest definition yyjson::value.
    static std::vector<PortEntry> PortsFromDefinitionList(
        const yyjson::value& definition,
        const std::string&   list_key); // "inputs" or "outputs"

    /// Resolve the declared (per-component_guid, cacheable) port surface:
    /// factory-manager manifest first, then the injected catalog provider.
    /// `was_resolved` stays false when neither source knows the component.
    ManifestPorts ResolveDeclaredPorts(
        const std::string&                              component_guid,
        std::unordered_map<std::string, ManifestPorts>& cache) const;

    /// Resolve the effective per-node port set: the declared surface of the
    /// node's referenced component (empty and unresolved when the target is
    /// not a componentRef or neither declaration source knows it), with the
    /// node's injected document faces and — for nodes holding a bound
    /// repository entry — the entry's faces merged on top (per-node union).
    ManifestPorts ResolveNodePorts(
        const Dto::GraphNodeDto&                        node,
        std::unordered_map<std::string, ManifestPorts>& cache) const;

    /// Merge one document-face list onto a port surface: dedupe by port id
    /// with the face winning over a same-name surviving port (WARN on the
    /// collision); each face joins only its declared direction side — except
    /// on begin/end boundary nodes, where `expand_pair` mirrors every face
    /// onto both sides (the boundary port-pair semantics: the in side
    /// receives the caller graph input, the out side feeds in-graph edges;
    /// the derivation layer excludes the role-mirrored side at D-14).
    /// Mutates only the caller's per-node copy — the guid-keyed declaration
    /// cache stays declared-only.
    void MergeFacesOntoSurface(
        ManifestPorts&                       ports,
        const std::string&                   node_id,
        const std::vector<DocumentFacePort>& faces,
        bool                                 expand_pair = false) const;

    /// Resolve the repository entry a node is bound to (nullopt = not a
    /// bound repository call node): the business-neutral judgment — a
    /// catalog-declared taskRepositoryRef field carrying a positive integer
    /// in the node settings. Returns the entry id as its decimal string
    /// (the EntryFacesMap key form).
    std::optional<std::string> ResolveBoundEntryId(
        const Dto::GraphNodeDto& node) const;

    /// Whether the node carries an injected faces entry (its own nodeFaces
    /// entry, or a bound repository entry with an entryFaces entry) — the
    /// stale-review eligibility marker distinguishing "the face evolved"
    /// from "the compile host did not run the prep pass".
    bool NodeCarriesInjectedFaces(const Dto::GraphNodeDto& node) const;

    /// Registry-extended type judgment for the stale review. Single
    /// authority preserved: the built-in equivalence table stays inside
    /// IsTypeCompatible and this wrapper only adds the closed-world rules
    /// of registered document types (same name or a compatibleWith link
    /// declared on either side is compatible; a judgeable undeclared pair
    /// is not). Pairs with an unjudgeable side (neither built-in nor
    /// registered — Core cannot enumerate plugin types) stay permissive by
    /// delegating to IsTypeCompatible.
    bool IsTypeCompatibleWithRegistry(
        const std::string& source_type,
        const std::string& target_type) const;

    /// Build a node index: node_id -> GraphNodeDto* for O(1) lookup.
    static std::unordered_map<std::string, const Dto::GraphNodeDto*>
    BuildNodeIndex(const Dto::GraphDocumentDto& document);
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHCOMPILER_H
