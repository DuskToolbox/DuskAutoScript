#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHCOMPILER_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHCOMPILER_H

#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphDocument.h>

#include <functional>
#include <optional>
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
    LoopBodyDataLeak   // Loop-body data edge leaves the loop (stale-value risk)
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

    /// Validate all edges in a GraphDocumentDto against node manifests.
    /// For each edge:
    ///   1. Verify source_node_id and target_node_id exist in graph
    ///   2. Resolve source node manifest -> check source_port_id in outputs
    ///   3. Resolve target node manifest -> check target_port_id in inputs
    ///   4. Check type compatibility between source output and target input
    /// Returns: vector of diagnostics (empty if all edges valid).
    std::vector<CompileEdgeDiagnostic> ValidateEdgePorts(
        const Dto::GraphDocumentDto& document);

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

    /// Bind the catalog-declared port surface provider. Extends (does not
    /// replace) factory-manager manifest resolution: edge validation,
    /// binding-plan generation and boundary derivation all consult it when
    /// the manifest path is unbound or unresolved.
    void SetPortSurfaceProvider(ComponentPortSurfaceProvider provider);

    /// Check type compatibility between source output and target input.
    /// The Core-side authoritative half of the numeric-equivalence table —
    /// the UI-side GraphConnectionRules.CanConnect (DasNativeUI) mirrors
    /// the same rules; the two sides must not drift.
    static bool IsTypeCompatible(
        const std::string& source_type,
        const std::string& target_type);

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
    /// not a componentRef or neither declaration source knows it).
    ManifestPorts ResolveNodePorts(
        const Dto::GraphNodeDto&                        node,
        std::unordered_map<std::string, ManifestPorts>& cache) const;

    /// Build a node index: node_id -> GraphNodeDto* for O(1) lookup.
    static std::unordered_map<std::string, const Dto::GraphNodeDto*>
    BuildNodeIndex(const Dto::GraphDocumentDto& document);

    /// Convert a CompileEdgeDiagnostic to a yyjson::value for plan
    /// diagnostics.
    static yyjson::value DiagnosticToJson(const CompileEdgeDiagnostic& diag);
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHCOMPILER_H
